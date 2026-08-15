#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>

#include "DebugData.h"
#include "PointerData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#define RESOLVE(name)                                                              \
    do {                                                                           \
        if (m_sys_##name == nullptr) {                                             \
            auto addr = ResolveLibcSymbol(#name);                                  \
            if (addr) {                                                            \
                m_sys_##name = reinterpret_cast<decltype(m_sys_##name)>(addr);     \
            }                                                                      \
        }                                                                          \
    } while (0)

struct InitState {
    InitState() { allocHook_setup = true; }
    ~InitState() { allocHook_setup = false; }
    static volatile bool allocHook_setup;
};
volatile bool InitState::allocHook_setup = false;

namespace {

volatile bool g_resolving_symbols = false;

struct alignas(std::max_align_t) BootstrapHeader {
    size_t map_size;
    uint64_t magic;
    BootstrapHeader* next;
};

constexpr uint64_t kBootstrapMagic = 0x6d616c6c6f635f68ULL;
BootstrapHeader* g_bootstrap_list = nullptr;

size_t PageAlign(size_t size) {
    long page_size = sysconf(_SC_PAGESIZE);
    size_t alignment = page_size > 0 ? static_cast<size_t>(page_size) : 4096;
    return (size + alignment - 1) & ~(alignment - 1);
}

bool IsBootstrapPointer(const void* ptr) {
    if (ptr == nullptr) {
        return false;
    }
    for (BootstrapHeader* header = g_bootstrap_list; header != nullptr; header = header->next) {
        if (header->magic == kBootstrapMagic && header + 1 == ptr) {
            return true;
        }
    }
    return false;
}

void* BootstrapMalloc(size_t size) {
    size_t map_size = PageAlign(sizeof(BootstrapHeader) + size);
    void* map = reinterpret_cast<void*>(syscall(
            SYS_mmap, nullptr, map_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (map == MAP_FAILED) {
        return nullptr;
    }
    auto* header = reinterpret_cast<BootstrapHeader*>(map);
    header->map_size = map_size;
    header->magic = kBootstrapMagic;
    header->next = g_bootstrap_list;
    g_bootstrap_list = header;
    return header + 1;
}

void BootstrapFree(void* ptr) {
    if (!IsBootstrapPointer(ptr)) {
        return;
    }
    auto* header = reinterpret_cast<BootstrapHeader*>(ptr) - 1;
    BootstrapHeader** current = &g_bootstrap_list;
    while (*current != nullptr && *current != header) {
        current = &(*current)->next;
    }
    if (*current == header) {
        *current = header->next;
    }
    size_t map_size = header->map_size;
    header->magic = 0;
    syscall(SYS_munmap, header, map_size);
}

void* BootstrapCalloc(size_t nmemb, size_t size) {
    size_t total = 0;
    if (__builtin_mul_overflow(nmemb, size, &total)) {
        errno = ENOMEM;
        return nullptr;
    }
    void* ptr = BootstrapMalloc(total);
    if (ptr != nullptr) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void* BootstrapRealloc(void* ptr, size_t size) {
    if (ptr == nullptr) {
        return BootstrapMalloc(size);
    }
    if (size == 0) {
        BootstrapFree(ptr);
        return nullptr;
    }
    auto* header = reinterpret_cast<BootstrapHeader*>(ptr) - 1;
    size_t old_size = IsBootstrapPointer(ptr) ? header->map_size - sizeof(BootstrapHeader) : 0;
    void* new_ptr = BootstrapMalloc(size);
    if (new_ptr != nullptr && old_size != 0) {
        memcpy(new_ptr, ptr, std::min(old_size, size));
        BootstrapFree(ptr);
    }
    return new_ptr;
}

void* ResolveLibcSymbol(const char* name) {
    if (g_resolving_symbols) {
        return nullptr;
    }

    g_resolving_symbols = true;
    void* addr = dlsym(RTLD_NEXT, name);
#if !defined(__MUSL__)
    if (addr == nullptr) {
        void* handle = dlopen("libc.so", RTLD_LAZY);
        if (handle) {
            addr = dlsym(handle, name);
            dlclose(handle);
        }
    }
#endif
    g_resolving_symbols = false;
    return addr;
}

void ResolveAllLibcAllocationSymbols() {
    RESOLVE(malloc);
    RESOLVE(free);
    RESOLVE(calloc);
    RESOLVE(realloc);
    RESOLVE(memalign);
    RESOLVE(aligned_alloc);
    RESOLVE(posix_memalign);
    RESOLVE(mmap);
    RESOLVE(munmap);
#if !defined(mmap64)
    RESOLVE(mmap64);
#endif
}

void* CallRealMmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    RESOLVE(mmap);
    if (m_sys_mmap != nullptr) {
        return m_sys_mmap(addr, size, prot, flags, fd, offset);
    }
    return reinterpret_cast<void*>(syscall(SYS_mmap, addr, size, prot, flags, fd, offset));
}

int CallRealMunmap(void* addr, size_t size) {
    RESOLVE(munmap);
    if (m_sys_munmap != nullptr) {
        return m_sys_munmap(addr, size);
    }
    return static_cast<int>(syscall(SYS_munmap, addr, size));
}

int CallRealIoctl(int fd, unsigned int request, void* arg) {
    return static_cast<int>(syscall(SYS_ioctl, fd, request, arg));
}

int CallRealClose(int fd) {
    return static_cast<int>(syscall(SYS_close, fd));
}

#if !defined(mmap64)
void* CallRealMmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    RESOLVE(mmap64);
    if (m_sys_mmap64 != nullptr) {
        return m_sys_mmap64(addr, size, prot, flags, fd, offset);
    }
    return CallRealMmap(addr, size, prot, flags, fd, offset);
}
#endif

struct AddressRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;

    bool Contains(const void* address) const {
        uintptr_t value = reinterpret_cast<uintptr_t>(address);
        return begin <= value && value < end;
    }

    bool Valid() const { return begin < end; }
};

AddressRange g_libc_range;

int CollectLibcRange(dl_phdr_info* info, size_t, void*) {
    if (info == nullptr || info->dlpi_name == nullptr) {
        return 0;
    }

    const char* so_name = strrchr(info->dlpi_name, '/');
    so_name = (so_name == nullptr) ? info->dlpi_name : so_name + 1;
    if (strcmp(so_name, "libc.so") != 0) {
        return 0;
    }

    uintptr_t begin = UINTPTR_MAX;
    uintptr_t end = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }

        uintptr_t seg_begin = info->dlpi_addr + phdr.p_vaddr;
        uintptr_t seg_end = seg_begin + phdr.p_memsz;
        if (seg_begin < begin) {
            begin = seg_begin;
        }
        if (seg_end > end) {
            end = seg_end;
        }
    }

    if (begin < end) {
        g_libc_range.begin = begin;
        g_libc_range.end = end;
        return 1;
    }
    return 0;
}

void CacheLibcRange() {
    if (!g_libc_range.Valid()) {
        dl_iterate_phdr(CollectLibcRange, nullptr);
    }
}

bool ShouldBypassLibcInternalMmap(const void* caller) {
    CacheLibcRange();
    return g_libc_range.Valid() && g_libc_range.Contains(caller);
}

}  // namespace

class AllocHook {
public:
    AllocHook() {
        InitState state;
        ResolveAllLibcAllocationSymbols();
        void* ptr[2] = {&Db_storage, &Pd_storage};
        debug_initialize(ptr);
        CacheLibcRange();
    }
    ~AllocHook() { debug_finalize(); }

    void* malloc(size_t size) { return debug_malloc(size); }
    void free(void* ptr) { debug_free(ptr); }
    void* calloc(size_t a, size_t b) { return debug_calloc(a, b); }
    void* realloc(void* ptr, size_t size) { return debug_realloc(ptr, size); }
    void* aligned_alloc(size_t alignment, size_t size) {
        return debug_aligned_alloc(alignment, size);
    }
    int posix_memalign(void** ptr, size_t alignment, size_t size) {
        return debug_posix_memalign(ptr, alignment, size);
    }
    void* memalign(size_t alignment, size_t bytes) { return debug_memalign(alignment, bytes); }
    void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return debug_mmap(addr, size, prot, flags, fd, offset);
    }
    int munmap(void* addr, size_t size) { return debug_munmap(addr, size); }
    int ioctl(int fd, int request, void* arg) { return debug_ioctl(fd, request, arg); }
    int close(int fd) { return debug_close(fd); }
#if !defined(mmap64)
    void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return debug_mmap64(addr, size, prot, flags, fd, offset);
    }
#endif

    void checkpoint(const char* file_name) { return debug_dump_heap(file_name); }
    void record_sample_peak(uint64_t epoch_ms, uint64_t dma_bytes, uint64_t rss_bytes) {
        debug_record_sample_peak(epoch_ms, dma_bytes, rss_bytes);
    }

    static AllocHook& inst();

private:
    static std::aligned_storage<sizeof(DebugData), alignof(DebugData)>::type Db_storage;
    static std::aligned_storage<sizeof(PointerData), alignof(PointerData)>::type
            Pd_storage;
};
std::aligned_storage<sizeof(DebugData), alignof(DebugData)>::type AllocHook::Db_storage;
std::aligned_storage<sizeof(PointerData), alignof(PointerData)>::type
        AllocHook::Pd_storage;

AllocHook& AllocHook::inst() {
    static AllocHook hook;
    return hook;
}

static volatile bool in_preinit_phase = true;
__attribute__((constructor(201))) void mark_init_done() {
    in_preinit_phase = false;
}

extern "C" {
// 程序初始化会间接调用 malloc 和 free
void* malloc(size_t size) {
    RESOLVE(malloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_malloc == nullptr) {
        if (m_sys_malloc == nullptr) {
            return BootstrapMalloc(size);
        }
        return m_sys_malloc(size);
    }
    return AllocHook::inst().malloc(size);
}

void free(void* ptr) {
    if (IsBootstrapPointer(ptr)) {
        BootstrapFree(ptr);
        return;
    }
    RESOLVE(free);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_free == nullptr) {
        if (m_sys_free == nullptr) {
            return;
        }
        return m_sys_free(ptr);
    }
    return AllocHook::inst().free(ptr);
}

// calloc 和 realloc 属于用户级函数
void* calloc(size_t a, size_t b) {
    RESOLVE(calloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_calloc == nullptr) {
        if (m_sys_calloc == nullptr) {
            return BootstrapCalloc(a, b);
        }
        return m_sys_calloc(a, b);
    }
    return AllocHook::inst().calloc(a, b);
}

void* realloc(void* ptr, size_t size) {
    bool bootstrap_ptr = IsBootstrapPointer(ptr);
    RESOLVE(realloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_realloc == nullptr || bootstrap_ptr) {
        if (m_sys_realloc == nullptr || bootstrap_ptr) {
            return BootstrapRealloc(ptr, size);
        }
        return m_sys_realloc(ptr, size);
    }
    return AllocHook::inst().realloc(ptr, size);
}

void* aligned_alloc(size_t alignment, size_t size) {
    RESOLVE(aligned_alloc);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_aligned_alloc == nullptr) {
        if (m_sys_aligned_alloc == nullptr) {
            return BootstrapMalloc(size);
        }
        return m_sys_aligned_alloc(alignment, size);
    }
    return AllocHook::inst().aligned_alloc(alignment, size);
}

void* memalign(size_t alignment, size_t bytes)  {
    RESOLVE(memalign);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_memalign == nullptr) {
        if (m_sys_memalign == nullptr) {
            return BootstrapMalloc(bytes);
        }
        return m_sys_memalign(alignment, bytes);
    }
    return AllocHook::inst().memalign(alignment, bytes);
}

// 进程初始化 和 debug init 的过程不应该调用 posix_memalign
int posix_memalign(void** ptr, size_t alignment, size_t size) {
    RESOLVE(memalign);
    RESOLVE(posix_memalign);
    if (InitState::allocHook_setup || g_resolving_symbols || m_sys_posix_memalign == nullptr) {
        if (m_sys_posix_memalign == nullptr) {
            *ptr = BootstrapMalloc(size);
            return (*ptr != nullptr) ? 0 : ENOMEM;
        }
        return m_sys_posix_memalign(ptr, alignment, size);
    }
    return AllocHook::inst().posix_memalign(ptr, alignment, size);
}

void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return CallRealMmap(addr, size, prot, flags, fd, offset);
    }
    if (fd < 0 && ShouldBypassLibcInternalMmap(__builtin_return_address(0))) {
        return CallRealMmap(addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap(addr, size, prot, flags, fd, offset);
    return result;
}

int munmap(void* addr, size_t size) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return CallRealMunmap(addr, size);
    }
    return AllocHook::inst().munmap(addr, size);
}

int ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    if (in_preinit_phase || InitState::allocHook_setup) {
        return CallRealIoctl(fd, static_cast<unsigned int>(request), arg);
    }
    return AllocHook::inst().ioctl(fd, request, arg);
}

int close(int fd) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return CallRealClose(fd);
    }
    return AllocHook::inst().close(fd);
}

#if !defined(mmap64)
void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return CallRealMmap64(addr, size, prot, flags, fd, offset);
    }
    if (fd < 0 && ShouldBypassLibcInternalMmap(__builtin_return_address(0))) {
        return CallRealMmap64(addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap64(addr, size, prot, flags, fd, offset);
    return result;
}
#endif

void checkpoint(const char* file_name) {
    AllocHook::inst().checkpoint(file_name);
}

void malloc_hook_record_sample_peak(
        uint64_t epoch_ms, uint64_t dma_bytes, uint64_t rss_bytes) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return;
    }
    AllocHook::inst().record_sample_peak(epoch_ms, dma_bytes, rss_bytes);
}
}

namespace {

void* HookOperatorNew(size_t size) {
    if (size == 0) {
        size = 1;
    }
    void* ptr = malloc(size);
    if (ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* HookOperatorNewNoThrow(size_t size) noexcept {
    try {
        return HookOperatorNew(size);
    } catch (...) {
        return nullptr;
    }
}

void* HookOperatorNewAligned(size_t size, std::align_val_t alignment) {
    if (size == 0) {
        size = 1;
    }
    void* ptr = nullptr;
    const size_t align = static_cast<size_t>(alignment);
    int ret = posix_memalign(&ptr, align, size);
    if (ret != 0 || ptr == nullptr) {
        throw std::bad_alloc();
    }
    return ptr;
}

void* HookOperatorNewAlignedNoThrow(size_t size, std::align_val_t alignment) noexcept {
    try {
        return HookOperatorNewAligned(size, alignment);
    } catch (...) {
        return nullptr;
    }
}

}  // namespace

void* operator new(size_t size) { return HookOperatorNew(size); }
void* operator new[](size_t size) { return HookOperatorNew(size); }
void* operator new(size_t size, const std::nothrow_t&) noexcept { return HookOperatorNewNoThrow(size); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return HookOperatorNewNoThrow(size); }

void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }
void operator delete(void* ptr, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, const std::nothrow_t&) noexcept { free(ptr); }

void* operator new(size_t size, std::align_val_t alignment) { return HookOperatorNewAligned(size, alignment); }
void* operator new[](size_t size, std::align_val_t alignment) { return HookOperatorNewAligned(size, alignment); }
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return HookOperatorNewAlignedNoThrow(size, alignment);
}
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return HookOperatorNewAlignedNoThrow(size, alignment);
}

void operator delete(void* ptr, std::align_val_t) noexcept { free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { free(ptr); }
void operator delete(void* ptr, size_t, std::align_val_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t, std::align_val_t) noexcept { free(ptr); }
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { free(ptr); }
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept { free(ptr); }
