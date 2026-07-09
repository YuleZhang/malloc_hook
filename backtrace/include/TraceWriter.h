#pragma once

#include <sys/types.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>

class TraceWriter {
public:
    static TraceWriter& Get();

    // Opens the trace_marker fd. Returns false if neither path works (tracing
    // is then silently disabled).  min_size/max_size=0 means no filter.
    bool Initialize(size_t min_size, size_t max_size);

    // Closes the trace_marker fd. Idempotent.
    void Shutdown();

    bool IsEnabled() const { return trace_fd_ >= 0; }

    // Write async begin (S) / end (F) event.
    // mem_type: 0=host, 1=mmap, 2=dma
    // hash_index: backtrace hash from PointerData, 0=untracked, included in the
    //         event name when >1 so the offline tooling can join richer info.
    void WriteAsyncBegin(int mem_type, size_t size, const void* ptr, size_t hash_index);
    void WriteAsyncEnd(int mem_type, size_t size, const void* ptr, size_t hash_index);

private:
    TraceWriter() = default;
    ~TraceWriter() = default;
    TraceWriter(const TraceWriter&) = delete;
    TraceWriter& operator=(const TraceWriter&) = delete;

    static int OpenTraceMarker();

    void WriteEvent(
            char marker, int mem_type, size_t size, const void* ptr, size_t hash_index);

    static int Uint64ToStr(uint64_t val, char* out);
    static const char* MemTypeName(int mem_type);

    // Format size with auto units: "128B", "4.5KB", "2.3MB". Returns chars written.
    static int FormatSizeToBuf(size_t bytes, char* out);

    int trace_fd_ = -1;
    pid_t tgid_ = 0;
    size_t min_size_ = 0;
    size_t max_size_ = SIZE_MAX;

    static constexpr size_t kMaxMsgLen = 256;
};
