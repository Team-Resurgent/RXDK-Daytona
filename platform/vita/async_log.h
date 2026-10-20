#pragma once

#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace vita {

// One owner submits periodic diagnostics; one worker writes them. The sink
// must outlive close(), which must run before SDL_Quit. Critical records and
// sink status reads remain synchronous: use sync(fn) or drain() before them.
// A full/unavailable queue drops periodic telemetry, never blocks the owner.
class AsyncLog {
public:
    static constexpr std::size_t kRecordCapacity = 1536;
    using Clock = uint64_t (*)();
    struct Stats {
        uint64_t submitted = 0, completed = 0, dropped = 0, failures = 0;
        uint64_t last_write_ticks = 0, max_write_ticks = 0;
        bool busy = false, threaded = false;
    };

    AsyncLog() = default;
    AsyncLog(const AsyncLog &) = delete;
    AsyncLog &operator=(const AsyncLog &) = delete;
    ~AsyncLog() { close(); }

    template<class Sink> bool open(Sink &sink, Clock clock = nullptr) {
        return open(&sink, [](void *context, const char *data, std::size_t size) {
            return static_cast<Sink *>(context)->append(data, size);
        }, clock);
    }
    bool threaded() const { return thread_ != nullptr; }

    // The one slot remains occupied until its write/sync/close has completed.
    // No allocation, waiting, or sink access occurs on this path.
    bool try_append(const char *data, std::size_t size) {
        if (!size) return true;
        if (!data || size > record_.size() || !thread_) { ++dropped_; return false; }
        if (SDL_TryLockMutex(mutex_) != 0) { ++dropped_; return false; }
        if (busy_ || stopping_) {
            SDL_UnlockMutex(mutex_);
            ++dropped_;
            return false;
        }
        std::memcpy(record_.data(), data, size);
        record_size_ = size;
        busy_ = true;
        ++submitted_;
        SDL_CondSignal(ready_);
        SDL_UnlockMutex(mutex_);
        return true;
    }

#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    bool try_log(const char *format, ...) {
        if (!format) { ++dropped_; return false; }
        char buffer[kRecordCapacity];
        va_list args;
        va_start(args, format);
        const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        if (n < 0) { ++dropped_; return false; }
        const std::size_t size = std::min(std::size_t(n), sizeof(buffer) - 1);
        if (std::size_t(n) >= sizeof(buffer)) {
            static constexpr char tail[] = " [truncated]\n";
            std::copy_n(tail, sizeof(tail) - 1, buffer + size - (sizeof(tail) - 1));
        }
        return try_append(buffer, size);
    }

    // Snapshot worker counters without waiting behind I/O or a contended mutex.
    // If its short critical section is busy, retain the previous observation.
    Stats stats() {
        if (thread_ && SDL_TryLockMutex(mutex_) == 0) {
            observe_locked();
            SDL_UnlockMutex(mutex_);
        }
        Stats result = observed_;
        result.submitted = submitted_;
        result.dropped = dropped_;
        result.threaded = threaded();
        return result;
    }

    // Owner-thread barrier: once it returns the worker cannot touch the sink
    // again until this same owner submits another record.
    void drain() noexcept {
        if (!thread_) return;
        SDL_LockMutex(mutex_);
        while (busy_) SDL_CondWait(completed_, mutex_);
        observe_locked();
        SDL_UnlockMutex(mutex_);
    }
    template<class Function> decltype(auto) sync(Function &&function) {
        drain();
        return std::forward<Function>(function)();
    }
    void close() noexcept {
        drain();
        if (thread_) {
            SDL_LockMutex(mutex_);
            stopping_ = true;
            SDL_CondSignal(ready_);
            SDL_UnlockMutex(mutex_);
            SDL_WaitThread(thread_, nullptr);
            thread_ = nullptr;
        }
        if (completed_) SDL_DestroyCond(completed_);
        if (ready_) SDL_DestroyCond(ready_);
        if (mutex_) SDL_DestroyMutex(mutex_);
        completed_ = ready_ = nullptr;
        mutex_ = nullptr;
        context_ = nullptr;
        write_ = nullptr;
    }

private:
    using Write = bool (*)(void *, const char *, std::size_t);
    SDL_Thread *thread_ = nullptr;
    SDL_mutex *mutex_ = nullptr;
    SDL_cond *ready_ = nullptr, *completed_ = nullptr;
    void *context_ = nullptr;
    Write write_ = nullptr;
    Clock clock_ = nullptr;
    std::array<char, kRecordCapacity> record_{};
    std::size_t record_size_ = 0;
    bool busy_ = false, stopping_ = false, initialized_ = false;
    // Owner-only counters and last observation; worker fields use the mutex.
    uint64_t submitted_ = 0, dropped_ = 0;
    Stats observed_;
    uint64_t completed_count_ = 0, failures_ = 0, last_write_ticks_ = 0, max_write_ticks_ = 0;

    void observe_locked() {
        observed_.completed = completed_count_;
        observed_.failures = failures_;
        observed_.last_write_ticks = last_write_ticks_;
        observed_.max_write_ticks = max_write_ticks_;
        observed_.busy = busy_;
    }
    bool open(void *context, Write write, Clock clock) {
        close();
        submitted_ = dropped_ = completed_count_ = failures_ = last_write_ticks_ = max_write_ticks_ = 0;
        observed_ = {};
        busy_ = stopping_ = initialized_ = false;
        context_ = context;
        write_ = write;
        clock_ = clock;
        mutex_ = SDL_CreateMutex();
        ready_ = SDL_CreateCond();
        completed_ = SDL_CreateCond();
        if (!mutex_ || !ready_ || !completed_) { close(); return false; }
        thread_ = SDL_CreateThreadWithStackSize(entry, "Daytona log", 64u * 1024u, this);
        if (!thread_) { close(); return false; }
        SDL_LockMutex(mutex_);
        while (!initialized_) SDL_CondWait(completed_, mutex_);
        SDL_UnlockMutex(mutex_);
        return true;
    }
    static int SDLCALL entry(void *opaque) {
        auto &self = *static_cast<AsyncLog *>(opaque);
        SDL_LockMutex(self.mutex_);
        self.initialized_ = true;
        SDL_CondSignal(self.completed_);
        for (;;) {
            while (!self.busy_ && !self.stopping_) SDL_CondWait(self.ready_, self.mutex_);
            if (self.stopping_) break;
            SDL_UnlockMutex(self.mutex_);
            bool ok = false;
            uint64_t elapsed = 0;
            try {
                const uint64_t begin = self.clock_ ? self.clock_() : 0;
                ok = self.write_(self.context_, self.record_.data(), self.record_size_);
                const uint64_t end = self.clock_ ? self.clock_() : 0;
                elapsed = end >= begin ? end - begin : 0;
            } catch (...) {
                // A failed diagnostic must not terminate the process. The
                // failed count is reported later by the owner thread.
                ok = false;
            }
            SDL_LockMutex(self.mutex_);
            ++self.completed_count_;
            if (!ok) ++self.failures_;
            self.last_write_ticks_ = elapsed;
            self.max_write_ticks_ = std::max(self.max_write_ticks_, elapsed);
            self.busy_ = false;
            SDL_CondSignal(self.completed_);
        }
        SDL_UnlockMutex(self.mutex_);
        return 0;
    }
};
} // namespace vita
