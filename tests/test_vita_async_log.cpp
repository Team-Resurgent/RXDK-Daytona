// Actual DiagnosticLog I/O against host mocks, with a real background thread.
// This checks queue ownership/order/lifetime, not Vita storage performance.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include "../platform/vita/async_log.h"
#include "../platform/vita/diagnostic_log.h"

struct SDL_mutex { std::mutex value; };
struct SDL_cond { std::condition_variable value; };
struct SDL_Thread { std::thread value; int result = 0; };
namespace mock {
int resources = 0, allocation = 0, fail_at = 0;
bool sdl_alive = true;
std::atomic<bool> try_contended{false};
std::mutex io_mutex;
std::condition_variable io_changed;
bool hold_io = false, entered_io = false, partial_write = false;
int io_fail = 0, active_files = 0, overlap = 0;
unsigned opens = 0, syncs = 0, closes = 0;
std::string file;
std::thread::id last_writer;
std::atomic<uint64_t> fake_time{0}, clock_step{7};

bool allocation_fails() { assert(sdl_alive); return ++allocation == fail_at; }
void reset(int failure = 0) {
    assert(resources == 0);
    allocation = 0; fail_at = failure; sdl_alive = true;
    try_contended = false; fake_time = 0; clock_step = 7;
    std::lock_guard lock(io_mutex);
    assert(active_files == 0);
    hold_io = entered_io = partial_write = false;
    io_fail = overlap = 0; opens = syncs = closes = 0;
    file.clear(); last_writer = {};
}
void block_io() { std::lock_guard lock(io_mutex); hold_io = true; entered_io = false; }
void await_io() {
    std::unique_lock lock(io_mutex);
    assert(io_changed.wait_for(lock, std::chrono::seconds(5), [] { return entered_io; }));
}
void release_io() { std::lock_guard lock(io_mutex); hold_io = false; io_changed.notify_all(); }
std::string contents() { std::lock_guard lock(io_mutex); return file; }
void failure(int value) { std::lock_guard lock(io_mutex); io_fail = value; }
uint64_t clock() { return fake_time.fetch_add(clock_step.load()); }
}

SDL_mutex *SDL_CreateMutex() {
    if (mock::allocation_fails()) return nullptr;
    ++mock::resources; return new SDL_mutex;
}
void SDL_DestroyMutex(SDL_mutex *p) { assert(mock::sdl_alive); --mock::resources; delete p; }
int SDL_LockMutex(SDL_mutex *p) { assert(mock::sdl_alive); p->value.lock(); return 0; }
int SDL_TryLockMutex(SDL_mutex *p) {
    assert(mock::sdl_alive);
    return !mock::try_contended && p->value.try_lock() ? 0 : 1;
}
int SDL_UnlockMutex(SDL_mutex *p) { assert(mock::sdl_alive); p->value.unlock(); return 0; }
SDL_cond *SDL_CreateCond() {
    if (mock::allocation_fails()) return nullptr;
    ++mock::resources; return new SDL_cond;
}
void SDL_DestroyCond(SDL_cond *p) { assert(mock::sdl_alive); --mock::resources; delete p; }
int SDL_CondWait(SDL_cond *c, SDL_mutex *m) {
    assert(mock::sdl_alive);
    std::unique_lock lock(m->value, std::adopt_lock);
    c->value.wait(lock); lock.release(); return 0;
}
int SDL_CondSignal(SDL_cond *c) { assert(mock::sdl_alive); c->value.notify_one(); return 0; }
SDL_Thread *SDL_CreateThreadWithStackSize(int (*entry)(void *), const char *, std::size_t stack, void *p) {
    assert(stack == 64u * 1024u);
    if (mock::allocation_fails()) return nullptr;
    auto *out = new SDL_Thread;
    out->value = std::thread([=] { out->result = entry(p); });
    ++mock::resources; return out;
}
void SDL_WaitThread(SDL_Thread *thread, int *status) {
    assert(mock::sdl_alive); thread->value.join();
    if (status) *status = thread->result;
    --mock::resources; delete thread;
}

extern "C" {
SceUID sceIoOpen(const char *, int flags, SceMode) {
    std::unique_lock lock(mock::io_mutex);
    ++mock::opens;
    if (mock::io_fail == 1) return -11;
    if (++mock::active_files != 1) ++mock::overlap;
    if (flags & SCE_O_TRUNC) mock::file.clear();
    mock::entered_io = true;
    mock::io_changed.notify_all();
    mock::io_changed.wait(lock, [] { return !mock::hold_io; });
    mock::last_writer = std::this_thread::get_id();
    return 42;
}
SceSSize sceIoWrite(SceUID fd, const void *data, SceSize size) {
    assert(fd == 42);
    std::lock_guard lock(mock::io_mutex);
    if (mock::io_fail == 2) return -12;
    if (mock::io_fail == 5) return 0;
    if (mock::partial_write) size = std::min(size, SceSize(3));
    mock::file.append(static_cast<const char *>(data), size);
    return int(size);
}
int sceIoSyncByFd(SceUID fd, int) {
    assert(fd == 42); std::lock_guard lock(mock::io_mutex); ++mock::syncs;
    return mock::io_fail == 3 ? -13 : 0;
}
int sceIoClose(SceUID fd) {
    assert(fd == 42); std::lock_guard lock(mock::io_mutex); ++mock::closes;
    --mock::active_files;
    return mock::io_fail == 4 ? -14 : 0;
}
} // extern "C"

namespace {
// Successful enqueue is eventually expected only in tests with an empty
// slot. The production API may legitimately reject transient lock contention.
template<class... Args>
void enqueue(vita::AsyncLog &log, const char *format, Args... args) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!log.try_log(format, args...)) {
        assert(log.threaded() && std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
void enqueue_raw(vita::AsyncLog &log, const char *data, std::size_t size) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!log.try_append(data, size)) {
        assert(log.threaded() && std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}
void test_order_and_overflow() {
    mock::reset();
    vita::DiagnosticLog sink;
    assert(sink.begin());
    const std::string header = mock::contents();
    vita::AsyncLog log;
    assert(log.open(sink, mock::clock));
    mock::block_io();
    char record[] = "perf one\n";
    enqueue_raw(log, record, sizeof(record) - 1);
    record[0] = 'X'; // Queue must own its copy before returning.
    mock::await_io();
    const uint64_t initial_drops = log.stats().dropped;
    for (int i = 0; i < 100; ++i) assert(!log.try_log("must drop %d\n", i));
    auto snapshot = log.stats();
    assert(snapshot.submitted == 1 && snapshot.dropped == initial_drops + 100 && snapshot.busy);
    std::thread unblock([] { mock::release_io(); });
    assert(log.sync([&] { return sink.literal("critical\n"); }));
    unblock.join();
    assert(mock::contents() == header + "perf one\ncritical\n");
    assert(mock::overlap == 0 && sink.error() == 0);
    snapshot = log.stats();
    assert(snapshot.completed == 1 && snapshot.failures == 0 && !snapshot.busy);
    assert(snapshot.last_write_ticks == 7 && snapshot.max_write_ticks == 7);

    mock::try_contended = true;
    assert(!log.try_log("mutex busy\n"));
    mock::try_contended = false;
    assert(log.stats().dropped == initial_drops + 101);
    mock::clock_step = 11;
    enqueue(log, "perf %d\n", 2);
    log.drain();
    snapshot = log.stats();
    assert(snapshot.completed == 2 && snapshot.last_write_ticks == 11 && snapshot.max_write_ticks == 11);
    assert(mock::last_writer != std::this_thread::get_id());
    assert(mock::contents() == header + "perf one\ncritical\nperf 2\n");
    log.close();
    assert(mock::resources == 0 && !log.threaded());
    assert(log.stats().completed == 2 && log.stats().dropped >= initial_drops + 101);
    mock::sdl_alive = false; // Repeated close/destruction must not call SDL.
    log.close();
}

void test_format_and_native_errors() {
    mock::reset();
    vita::DiagnosticLog sink;
    vita::AsyncLog log;
    assert(log.open(sink));
    std::string long_record(4096, 'a');
    enqueue(log, "%s", long_record.c_str());
    log.drain();
    auto text = mock::contents();
    assert(text.size() == vita::AsyncLog::kRecordCapacity - 1);
    assert(text.ends_with(" [truncated]\n"));
    assert(log.try_append(nullptr, 0));
    const uint64_t before_invalid = log.stats().dropped;
    assert(!log.try_append(nullptr, 1));
    assert(!log.try_append(long_record.data(), long_record.size()));
    assert(!log.try_log(nullptr));
    assert(log.stats().dropped == before_invalid + 3);
    {
        std::lock_guard lock(mock::io_mutex); mock::partial_write = true;
    }
    enqueue(log, "partial write is completed\n");
    log.drain();
    assert(mock::contents().ends_with("partial write is completed\n"));
    log.close();
    assert(mock::resources == 0);

    for (int failure = 1; failure <= 5; ++failure) {
        mock::reset();
        vita::DiagnosticLog failed_sink;
        vita::AsyncLog worker;
        assert(worker.open(failed_sink));
        mock::failure(failure);
        enqueue(worker, "failed %d\n", failure);
        worker.drain();
        const auto snapshot = worker.stats();
        assert(snapshot.completed == 1);
        // DiagnosticLog reports sync warnings separately from failed writes.
        assert(snapshot.failures == (failure == 3 ? 0u : 1u));
        assert(failed_sink.sync_error() == (failure == 3 ? -13 : 0));
        assert(failed_sink.error() == (failure == 3 ? 0 : failure == 5 ? -1 : -10 - failure));
        mock::failure(0);
        enqueue(worker, "recovered\n");
        worker.close();
        assert(mock::contents().ends_with("recovered\n"));
        assert(mock::active_files == 0 && mock::overlap == 0 && mock::resources == 0);
    }
}

void test_fallback_and_lifecycle() {
    for (int failure = 1; failure <= 4; ++failure) {
        mock::reset(failure);
        vita::DiagnosticLog sink;
        vita::AsyncLog log;
        assert(!log.open(sink));
        assert(mock::resources == 0 && !log.threaded());
        assert(!log.try_log("periodic not synchronous\n"));
        assert(mock::opens == 0 && log.stats().dropped == 1);
        assert(log.sync([&] { return sink.literal("critical fallback\n"); }));
        assert(mock::contents() == "critical fallback\n");
        assert(mock::last_writer == std::this_thread::get_id());
        log.close();
    }
    mock::reset();
    vita::DiagnosticLog sink;
    {
        vita::AsyncLog log;
        assert(log.open(sink));
        mock::block_io();
        enqueue(log, "drain before shutdown\n");
        mock::await_io();
        std::thread unblock([] { mock::release_io(); });
        log.close();
        unblock.join();
        assert(mock::contents() == "drain before shutdown\n");
        assert(mock::resources == 0);
        mock::sdl_alive = false;
    }
    mock::reset();
    {
        vita::AsyncLog log;
        assert(log.open(sink));
        enqueue(log, "destructor joins\n");
    }
    assert(mock::contents() == "destructor joins\n" && mock::resources == 0);
}

void test_sink_exceptions_and_reopen() {
    struct Sink {
        int calls = 0;
        bool append(const char *, std::size_t) {
            ++calls;
            if (calls == 1) throw std::runtime_error("mock sink failure");
            return calls != 2;
        }
    } sink;
    mock::reset();
    vita::AsyncLog log;
    assert(log.open(sink));
    for (int i = 0; i < 3; ++i) { enqueue(log, "record\n"); log.drain(); }
    assert(log.stats().completed == 3 && log.stats().failures == 2 && sink.calls == 3);
    assert(log.open(sink)); // Reopen closes/joins old worker and resets counters.
    assert(log.stats().completed == 0 && log.stats().submitted == 0);
    enqueue(log, "new worker\n");
    log.close();
    assert(sink.calls == 4 && log.stats().completed == 1 && mock::resources == 0);
}

void test_stress() {
    struct Sink {
        uint64_t records = 0;
        bool append(const char *data, std::size_t size) {
            assert(size == 16);
            for (std::size_t i = 1; i < size; ++i) assert(data[i] == data[0]);
            ++records;
            return true;
        }
    } sink;
    mock::reset();
    vita::AsyncLog log;
    assert(log.open(sink));
    for (unsigned i = 0; i < 20000; ++i) {
        std::array<char, 16> record;
        record.fill(char(i));
        log.try_append(record.data(), record.size());
        if (i % 97 == 0) {
            log.drain();
            assert(log.stats().completed == sink.records);
        }
        if (i % 11 == 0) (void)log.stats();
    }
    log.close();
    const auto stats = log.stats();
    assert(stats.submitted + stats.dropped == 20000);
    assert(stats.completed == stats.submitted && stats.completed == sink.records);
    assert(stats.failures == 0 && mock::resources == 0);
}
}

int main() {
    test_order_and_overflow();
    test_format_and_native_errors();
    test_fallback_and_lifecycle();
    test_sink_exceptions_and_reopen();
    test_stress();
    std::puts("Vita async log: ordering, bounded drops, native errors, fallback and lifecycle passed");
}
