#include "TraceWriter.h"

#include <fcntl.h>
#include <sys/syscall.h>

TraceWriter& TraceWriter::Get() {
    static TraceWriter instance;
    return instance;
}

bool TraceWriter::Initialize(size_t min_size, size_t max_size) {
    trace_fd_ = OpenTraceMarker();
    if (trace_fd_ < 0)
        return false;
    tgid_ = static_cast<pid_t>(syscall(SYS_getpid));
    min_size_ = min_size;
    max_size_ = max_size;
    return true;
}

void TraceWriter::Shutdown() {
    if (trace_fd_ >= 0) {
        syscall(SYS_close, trace_fd_);
        trace_fd_ = -1;
    }
}

void TraceWriter::WriteAsyncBegin(
        int mem_type, size_t size, const void* ptr, size_t hash_index) {
    WriteEvent('S', mem_type, size, ptr, hash_index);
}

void TraceWriter::WriteAsyncEnd(
        int mem_type, size_t size, const void* ptr, size_t hash_index) {
    WriteEvent('F', mem_type, size, ptr, hash_index);
}

int TraceWriter::OpenTraceMarker() {
    int fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, "/sys/kernel/tracing/trace_marker",
                    O_WRONLY | O_CLOEXEC, 0));
    if (fd >= 0)
        return fd;

    fd = static_cast<int>(
            syscall(SYS_openat, AT_FDCWD, "/sys/kernel/debug/tracing/trace_marker",
                    O_WRONLY | O_CLOEXEC, 0));
    return fd;
}

void TraceWriter::WriteEvent(
        char marker, int mem_type, size_t size, const void* ptr, size_t hash_index) {
    if (trace_fd_ < 0)
        return;
    if (size < min_size_ || size > max_size_)
        return;

    char buf[kMaxMsgLen];
    int pos = 0;

    // "S|<tgid>|memory_<type>.<size>@<ptr>.h<hash>|0\n"
    buf[pos++] = marker;
    buf[pos++] = '|';
    pos += Uint64ToStr(static_cast<uint64_t>(tgid_), buf + pos);
    buf[pos++] = '|';

    // "memory_" prefix for searchability in Perfetto UI
    const char* prefix = "memory_";
    while (*prefix)
        buf[pos++] = *prefix++;

    const char* type_str = MemTypeName(mem_type);
    while (*type_str)
        buf[pos++] = *type_str++;

    buf[pos++] = '.';
    pos += FormatSizeToBuf(size, buf + pos);

    buf[pos++] = '@';
    pos += Uint64ToStr(reinterpret_cast<uintptr_t>(ptr), buf + pos);

    // Include hash_index so the offline tooling can join richer allocation info
    // (variable / call site / call path) back to this event.
    if (hash_index > 1) {
        buf[pos++] = '.';
        buf[pos++] = 'h';
        pos += Uint64ToStr(static_cast<uint64_t>(hash_index), buf + pos);
    }

    buf[pos++] = '|';
    buf[pos++] = '0';  // fixed cookie
    buf[pos++] = '\n';

    syscall(SYS_write, trace_fd_, buf, pos);
}

int TraceWriter::FormatSizeToBuf(size_t bytes, char* out) {
    if (bytes < 1024) {
        // e.g. "128B"
        int n = Uint64ToStr(static_cast<uint64_t>(bytes), out);
        out[n++] = 'B';
        return n;
    }

    if (bytes < 1024 * 1024) {
        // e.g. "4.5KB" or "500KB"
        size_t kb = bytes / 1024;
        size_t rem = bytes % 1024;
        int n = Uint64ToStr(static_cast<uint64_t>(kb), out);
        if (kb < 10 && rem > 0) {
            // One decimal digit
            out[n++] = '.';
            out[n++] = '0' + static_cast<char>((rem * 10) / 1024);
        }
        out[n++] = 'K';
        out[n++] = 'B';
        return n;
    }

    // >= 1MB, e.g. "2.3MB" or "200MB"
    size_t mb = bytes / (1024 * 1024);
    size_t rem = bytes % (1024 * 1024);
    int n = Uint64ToStr(static_cast<uint64_t>(mb), out);
    if (mb < 10 && rem > 0) {
        out[n++] = '.';
        out[n++] = '0' + static_cast<char>((rem * 10) / (1024 * 1024));
    }
    out[n++] = 'M';
    out[n++] = 'B';
    return n;
}

int TraceWriter::Uint64ToStr(uint64_t val, char* out) {
    if (val == 0) {
        out[0] = '0';
        return 1;
    }
    char tmp[20];
    int len = 0;
    while (val > 0) {
        tmp[len++] = '0' + static_cast<char>(val % 10);
        val /= 10;
    }
    for (int i = 0; i < len; i++) {
        out[i] = tmp[len - 1 - i];
    }
    return len;
}

const char* TraceWriter::MemTypeName(int mem_type) {
    static const char* names[] = {"host", "mmap", "dma"};
    if (mem_type < 0 || mem_type > 2)
        return "unknown";
    return names[mem_type];
}
