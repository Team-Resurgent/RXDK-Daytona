// Serialized native diagnostic sink, independent of newlib FILE/SDL logging.
// Callers synchronize all writes and status reads when using a worker thread.
#pragma once
#include <psp2/io/fcntl.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>

namespace vita {
class DiagnosticLog {
public:
    static constexpr size_t kLimit = 1024 * 1024;
    static constexpr const char *kPath = "ux0:data/daytona93/vita-diag.log";

    explicit DiagnosticLog(bool enabled = true) : enabled_(enabled) {}
    bool enabled() const { return enabled_; }

    // Safe before the heap probe: constant data and native I/O only.
    bool begin() {
        if (!enabled_) return true;
        bytes_ = 0; error_ = sync_error_ = 0; limited_ = false;
        static constexpr char header[] = "DAYTONA VITA OPT03 - direct native file logging\n";
        return write(header, sizeof(header) - 1, true);
    }
    bool append(const char *data, size_t size) { return !enabled_ || write(data, size, false); }
    template<size_t N> bool literal(const char (&data)[N]) { return append(data, N - 1); }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    bool log(const char *format, ...) {
        if (!enabled_) return true;
        va_list args;
        va_start(args, format);
        const bool ok = formatted(format, args);
        va_end(args);
        return ok;
    }
    // Unexpected faults remain available in quiet builds. Append only: a
    // normal quiet startup must never erase the previous diagnostic evidence.
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    bool fault(const char *format, ...) {
        va_list args;
        va_start(args, format);
        const bool ok = formatted(format, args);
        va_end(args);
        return ok;
    }
    int error() const { return error_; }
    int sync_error() const { return sync_error_; }
    bool limited() const { return limited_; }
    size_t bytes() const { return bytes_; }
    const char *state() const {
        return error_ ? "ERROR" : sync_error_ ? "SYNC WARNING" : limited_ ? "LIMIT" : enabled_ ? "OK" : "OFF";
    }

private:
    bool formatted(const char *format, va_list args) {
        if (!format) { remember(-1); return false; }
        char buffer[1536];
        const int n = std::vsnprintf(buffer, sizeof(buffer), format, args);
        if (n < 0) { remember(-1); return false; }
        // Make truncation explicit and retain a line boundary.
        size_t size = std::min(size_t(n), sizeof(buffer) - 1);
        if (size_t(n) >= sizeof(buffer)) {
            static constexpr char tail[] = " [truncated]\n";
            std::copy_n(tail, sizeof(tail) - 1, buffer + size - (sizeof(tail) - 1));
        }
        return write(buffer, size, false);
    }
    void remember(int error) { if (!error_) error_ = error; }
    bool write(const char *data, size_t size, bool truncate) {
        if (!size) return true;
        if (!data) { remember(-1); return false; }
        if (size > kLimit - bytes_) { limited_ = true; return false; }
        const SceUID fd = sceIoOpen(kPath, SCE_O_WRONLY | SCE_O_CREAT |
                                   (truncate ? SCE_O_TRUNC : SCE_O_APPEND), 0666);
        if (fd < 0) { remember(fd); return false; }
        bool ok = true;
        while (size) {
            const int n = sceIoWrite(fd, data, size);
            if (n <= 0 || size_t(n) > size) {
                remember(n < 0 ? n : -1); ok = false; break;
            }
            data += n; size -= size_t(n); bytes_ += size_t(n);
        }
        // A failed sync is distinguished from a failed write. Closing each
        // record avoids a long-lived userspace stdio buffer or shared offset.
        const int synced = sceIoSyncByFd(fd, 0);
        if (synced < 0 && !sync_error_) sync_error_ = synced;
        const int closed = sceIoClose(fd);
        if (closed < 0) { remember(closed); ok = false; }
        return ok;
    }
    const bool enabled_;
    size_t bytes_ = 0;
    int error_ = 0, sync_error_ = 0;
    bool limited_ = false;
};
} // namespace vita
