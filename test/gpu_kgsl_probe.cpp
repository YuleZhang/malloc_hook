// kgsl_probe: compare /proc/self/smaps GPU accounting against the KGSL
// per-process sysfs nodes (gpumem_mapped / gpumem_unmapped / imported_mem)
// across the OpenCL allocation lifecycle, on Qualcomm/Adreno.
//
// Built on third_party/opencl_stub: it supplies the cl* symbols by dlopen()ing
// the device's real libOpenCL.so, so this program calls the OpenCL API directly
// and the stub forwards to the driver.
//
// One scenario per process run (kgsl sysfs nodes are process-cumulative, so a
// fresh process keeps each scenario's deltas clean). It prints one SNAP line per
// lifecycle step; a driver holds its own working set from context creation, so
// read the deltas between SNAP lines, never the absolutes.

#include "CL/cl.h"
#include "CL/cl_ext.h"
#include "CL/cl_ext_qcom.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ------------------------------------------------------------------ snapshot

struct Snap {
    long vmrss_kb = 0;         // VmRSS from /proc/self/status
    long rss_anon_kb = 0;      // RssAnon
    long rss_file_kb = 0;      // RssFile
    long rss_shmem_kb = 0;     // RssShmem
    long sum_rss_kb = 0;       // Σ per-VMA Rss over all mappings (smaps)
    long dev_size_kb = 0;      // Σ Size over /dev/kgsl mappings
    long dev_rss_kb = 0;       // Σ Rss  over /dev/kgsl mappings
    long dmabuf_size_kb = 0;   // Σ Size over dma-buf mappings
    long dmabuf_rss_kb = 0;    // Σ Rss  over dma-buf mappings
    // KGSL sysfs proc nodes, converted to kB
    long k_mapped_kb = 0;
    long k_unmapped_kb = 0;
    long k_kernel_kb = 0;      // == mapped + unmapped on these kernels
    long k_kernelmax_kb = 0;
    long k_imported_kb = 0;
    long k_reclaimed_kb = 0;   // pages the driver unpinned under pressure (newer)
};

// Read VmRSS plus the RssAnon/RssFile/RssShmem breakdown in one pass.
static void read_status(Snap& s) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[256];
    long v;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmRSS:", 6) == 0) { sscanf(line + 6, "%ld", &v); s.vmrss_kb = v; }
        else if (strncmp(line, "RssAnon:", 8) == 0) { sscanf(line + 8, "%ld", &v); s.rss_anon_kb = v; }
        else if (strncmp(line, "RssFile:", 8) == 0) { sscanf(line + 8, "%ld", &v); s.rss_file_kb = v; }
        else if (strncmp(line, "RssShmem:", 9) == 0) { sscanf(line + 9, "%ld", &v); s.rss_shmem_kb = v; }
    }
    fclose(f);
}

// Classify a mapping by its pathname (last column of a smaps header line).
enum Kind { OTHER, DEV_KGSL, DMABUF };
static Kind classify(const char* path) {
    if (!path || !*path) return OTHER;
    if (strstr(path, "/dev/kgsl")) return DEV_KGSL;
    if (strstr(path, "kgsl-3d0")) return DEV_KGSL;
    if (strstr(path, "dmabuf")) return DMABUF;      // "/dmabuf:..." or "anon_inode:dmabuf"
    if (strstr(path, "/dev/dma_heap")) return DMABUF;
    return OTHER;
}

static void read_smaps(Snap& s) {
    FILE* f = fopen("/proc/self/smaps", "r");
    if (!f) return;
    char line[512];
    Kind cur = OTHER;
    while (fgets(line, sizeof(line), f)) {
        // Header line looks like: addr-addr perms off dev inode  path
        // We detect it by the '-' separated hex address at column 0.
        if ((line[0] >= '0' && line[0] <= '9') ||
            (line[0] >= 'a' && line[0] <= 'f')) {
            // find pathname: after the 5th field. Simplify: locate first '/'
            // or '[' after the inode; otherwise anonymous.
            const char* path = "";
            // skip: address perms offset dev inode
            char* p = line;
            int fields = 0;
            while (*p && fields < 5) {
                while (*p == ' ') ++p;
                while (*p && *p != ' ') ++p;
                ++fields;
            }
            while (*p == ' ') ++p;
            // strip newline
            char* nl = strchr(p, '\n');
            if (nl) *nl = 0;
            path = p;
            cur = classify(path);
            continue;
        }
        long v;
        if (strncmp(line, "Rss:", 4) == 0) {
            sscanf(line + 4, "%ld", &v);
            s.sum_rss_kb += v;
            if (cur == DEV_KGSL) s.dev_rss_kb += v;
            else if (cur == DMABUF) s.dmabuf_rss_kb += v;
        } else if (strncmp(line, "Size:", 5) == 0) {
            sscanf(line + 5, "%ld", &v);
            if (cur == DEV_KGSL) s.dev_size_kb += v;
            else if (cur == DMABUF) s.dmabuf_size_kb += v;
        }
    }
    fclose(f);
}

static long read_kgsl_node_bytes(const char* name) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/kgsl/kgsl/proc/%d/%s", getpid(), name);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%ld", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static Snap take_snapshot() {
    Snap s;
    read_status(s);
    read_smaps(s);
    long m = read_kgsl_node_bytes("gpumem_mapped");
    long u = read_kgsl_node_bytes("gpumem_unmapped");
    long k = read_kgsl_node_bytes("kernel");
    long km = read_kgsl_node_bytes("kernel_max");
    long im = read_kgsl_node_bytes("imported_mem");
    long rc = read_kgsl_node_bytes("gpumem_reclaimed");
    s.k_mapped_kb = m < 0 ? -1 : m / 1024;
    s.k_unmapped_kb = u < 0 ? -1 : u / 1024;
    s.k_kernel_kb = k < 0 ? -1 : k / 1024;
    s.k_kernelmax_kb = km < 0 ? -1 : km / 1024;
    s.k_imported_kb = im < 0 ? -1 : im / 1024;
    s.k_reclaimed_kb = rc < 0 ? -1 : rc / 1024;
    return s;
}

static void print_snap(const char* label, const Snap& s) {
    printf("SNAP %-16s vmrss=%ld anon=%ld file=%ld shmem=%ld sumrss=%ld "
           "dev_size=%ld dev_rss=%ld dmabuf_size=%ld dmabuf_rss=%ld | "
           "k_mapped=%ld k_unmapped=%ld k_kernel=%ld k_kmax=%ld "
           "k_imported=%ld k_reclaimed=%ld\n",
           label, s.vmrss_kb, s.rss_anon_kb, s.rss_file_kb, s.rss_shmem_kb,
           s.sum_rss_kb, s.dev_size_kb, s.dev_rss_kb, s.dmabuf_size_kb,
           s.dmabuf_rss_kb, s.k_mapped_kb, s.k_unmapped_kb, s.k_kernel_kb,
           s.k_kernelmax_kb, s.k_imported_kb, s.k_reclaimed_kb);
    fflush(stdout);
}

// ------------------------------------------------------------------ helpers

static const char* errstr(cl_int e) {
    static char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)e);
    return buf;
}

#define CK(expr)                                                        \
    do {                                                                \
        cl_int _e = (expr);                                             \
        if (_e != CL_SUCCESS) {                                         \
            fprintf(stderr, "FAIL %s -> %s\n", #expr, errstr(_e));      \
            exit(2);                                                    \
        }                                                               \
    } while (0)

static int dma_heap_alloc(size_t bytes) {
    int hfd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    if (hfd < 0) {
        perror("open /dev/dma_heap/system");
        return -1;
    }
    struct dma_heap_allocation_data data;
    memset(&data, 0, sizeof(data));
    data.len = bytes;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    int r = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &data);
    close(hfd);
    if (r < 0) {
        perror("DMA_HEAP_IOCTL_ALLOC");
        return -1;
    }
    return (int)data.fd;
}

// ------------------------------------------------------------------ scenarios

int main(int argc, char** argv) {
    std::string scenario = argc > 1 ? argv[1] : "rw";
    size_t mb = argc > 2 ? (size_t)atol(argv[2]) : 64;
    size_t bytes = mb * 1024 * 1024;

    cl_platform_id plat;
    cl_uint np = 0;
    CK(clGetPlatformIDs(1, &plat, &np));
    cl_device_id dev;
    cl_uint nd = 0;
    CK(clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 1, &dev, &nd));
    cl_int err;
    cl_context ctx = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
    CK(err);
    cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err);
    CK(err);

    printf("# scenario=%s size=%zuMB pid=%d\n", scenario.c_str(), mb, getpid());
    print_snap("baseline_ctx", take_snapshot());

    auto do_buffer = [&](cl_mem_flags flags, void* host_ptr) {
        cl_mem buf = clCreateBuffer(ctx, flags, bytes, host_ptr, &err);
        CK(err);
        print_snap("after_create", take_snapshot());
        void* mapped = clEnqueueMapBuffer(q, buf, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE,
                                          0, bytes, 0, nullptr, nullptr, &err);
        CK(err);
        print_snap("after_map", take_snapshot());
        memset(mapped, 0xAB, bytes);
        clFinish(q);
        print_snap("after_touch", take_snapshot());
        CK(clEnqueueUnmapMemObject(q, buf, mapped, 0, nullptr, nullptr));
        clFinish(q);
        print_snap("after_unmap", take_snapshot());
        CK(clReleaseMemObject(buf));
        clFinish(q);
        print_snap("after_release", take_snapshot());
    };

    if (scenario == "rw") {
        do_buffer(CL_MEM_READ_WRITE, nullptr);
    } else if (scenario == "allochost") {
        do_buffer(CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR, nullptr);
    } else if (scenario == "usehost") {
        void* hp = nullptr;
        posix_memalign(&hp, 4096, bytes);
        memset(hp, 0x11, bytes);  // resident host pages before import
        do_buffer(CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, hp);
    } else if (scenario == "props") {
        // clCreateBufferWithProperties, empty property list == clCreateBuffer.
        cl_mem_properties props[] = {0};
        cl_mem buf = clCreateBufferWithProperties(ctx, props, CL_MEM_READ_WRITE,
                                                  bytes, nullptr, &err);
        CK(err);
        print_snap("after_create", take_snapshot());
        void* mapped = clEnqueueMapBuffer(q, buf, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE,
                                          0, bytes, 0, nullptr, nullptr, &err);
        CK(err);
        print_snap("after_map", take_snapshot());
        memset(mapped, 0xCD, bytes);
        clFinish(q);
        print_snap("after_touch", take_snapshot());
        CK(clReleaseMemObject(buf));
        print_snap("after_release", take_snapshot());
    } else if (scenario == "import_dmabuf" || scenario == "import_ion") {
        int fd = dma_heap_alloc(bytes);
        if (fd < 0) { fprintf(stderr, "dma-heap alloc failed\n"); return 3; }
        void* hp = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (hp == MAP_FAILED) { perror("mmap dmabuf"); return 3; }
        memset(hp, 0x22, bytes);  // make the dma-buf pages resident
        print_snap("after_dmabuf_touch", take_snapshot());

        cl_mem_flags flags = CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM;
        cl_mem buf;
        if (scenario == "import_dmabuf") {
            cl_mem_dmabuf_host_ptr dhp;
            memset(&dhp, 0, sizeof(dhp));
            dhp.ext_host_ptr.allocation_type = CL_MEM_DMABUF_HOST_PTR_QCOM;
            dhp.ext_host_ptr.host_cache_policy = CL_MEM_HOST_UNCACHED_QCOM;
            dhp.dmabuf_filedesc = fd;
            dhp.dmabuf_hostptr = hp;
            buf = clCreateBuffer(ctx, flags, bytes, &dhp, &err);
        } else {
            cl_mem_ion_host_ptr ihp;
            memset(&ihp, 0, sizeof(ihp));
            ihp.ext_host_ptr.allocation_type = CL_MEM_ION_HOST_PTR_QCOM;
            ihp.ext_host_ptr.host_cache_policy = CL_MEM_HOST_UNCACHED_QCOM;
            ihp.ion_filedesc = fd;
            ihp.ion_hostptr = hp;
            buf = clCreateBuffer(ctx, flags, bytes, &ihp, &err);
        }
        CK(err);
        print_snap("after_import", take_snapshot());
        void* mapped = clEnqueueMapBuffer(q, buf, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE,
                                          0, bytes, 0, nullptr, nullptr, &err);
        CK(err);
        print_snap("after_map", take_snapshot());
        memset(mapped, 0x33, bytes);
        clFinish(q);
        print_snap("after_touch", take_snapshot());
        CK(clReleaseMemObject(buf));
        print_snap("after_release", take_snapshot());
    } else if (scenario == "leak") {
        // Create N buffers, never release, then release all. Demonstrates whether
        // the KGSL kernel node tracks create-without-release growth.
        int n = 4;
        std::vector<cl_mem> bufs;
        for (int i = 0; i < n; ++i) {
            cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
            CK(err);
            void* m = clEnqueueMapBuffer(q, b, CL_TRUE, CL_MAP_WRITE, 0, bytes, 0,
                                         nullptr, nullptr, &err);
            CK(err);
            memset(m, i + 1, bytes);
            clFinish(q);
            bufs.push_back(b);
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "after_create_%d", i + 1);
            print_snap(lbl, take_snapshot());
        }
        for (cl_mem b : bufs) CK(clReleaseMemObject(b));
        clFinish(q);
        print_snap("after_release_all", take_snapshot());
    } else if (scenario == "multimap") {
        // Map the same buffer M times without unmapping. Demonstrates whether a
        // repeated map is visible (double-map leak class).
        cl_mem buf = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
        CK(err);
        print_snap("after_create", take_snapshot());
        std::vector<void*> maps;
        for (int i = 0; i < 3; ++i) {
            void* m = clEnqueueMapBuffer(q, buf, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE,
                                         0, bytes, 0, nullptr, nullptr, &err);
            CK(err);
            memset(m, 0x40 + i, bytes);
            clFinish(q);
            maps.push_back(m);
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "after_map_%d", i + 1);
            print_snap(lbl, take_snapshot());
        }
        for (void* m : maps) clEnqueueUnmapMemObject(q, buf, m, 0, nullptr, nullptr);
        clFinish(q);
        print_snap("after_unmap_all", take_snapshot());
        CK(clReleaseMemObject(buf));
        print_snap("after_release", take_snapshot());
    } else if (scenario == "svm") {
        // Coarse-grain SVM: clSVMAlloc, then map to the CPU, touch, unmap. Tests
        // whether SVM memory lands in the same gpumem 'kernel' node as buffers.
        void* svm = clSVMAlloc(ctx, CL_MEM_READ_WRITE, bytes, 0);
        if (svm == nullptr) { fprintf(stderr, "clSVMAlloc returned NULL\n"); return 4; }
        print_snap("after_svmalloc", take_snapshot());
        CK(clEnqueueSVMMap(q, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, svm, bytes, 0,
                           nullptr, nullptr));
        clFinish(q);
        print_snap("after_svmmap", take_snapshot());
        memset(svm, 0x55, bytes);
        clFinish(q);
        print_snap("after_touch", take_snapshot());
        CK(clEnqueueSVMUnmap(q, svm, 0, nullptr, nullptr));
        clFinish(q);
        print_snap("after_svmunmap", take_snapshot());
        clSVMFree(ctx, svm);
        clFinish(q);
        print_snap("after_svmfree", take_snapshot());
    } else if (scenario == "sizefit") {
        // Size fidelity: request a series of sizes and print the kgsl 'kernel'
        // delta against the requested bytes, to expose page rounding / padding.
        // Each sub-allocation is created, mapped+touched, snapshotted, released.
        const size_t reqs[] = {1, 4095, 4096, 4097, 100000, 1048576,
                               1048576 + 1, 63u * 1024 * 1024 + 123};
        for (size_t r : reqs) {
            cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE, r, nullptr, &err);
            CK(err);
            void* m = clEnqueueMapBuffer(q, b, CL_TRUE, CL_MAP_WRITE, 0, r, 0,
                                         nullptr, nullptr, &err);
            CK(err);
            memset(m, 1, r);
            clFinish(q);
            char lbl[40];
            snprintf(lbl, sizeof(lbl), "req_%zu", r);
            print_snap(lbl, take_snapshot());
            CK(clEnqueueUnmapMemObject(q, b, m, 0, nullptr, nullptr));
            CK(clReleaseMemObject(b));
            clFinish(q);
        }
    } else if (scenario == "hold_touched" || scenario == "hold_untouched") {
        // Allocate several buffers, optionally touch them, then sleep so a
        // timer-driven external sampler (ALLOC_HOOK_PEAK_SAMPLE_MS) catches the
        // held state. Prints the KGSL ground truth for the held peak.
        const bool touch = (scenario == "hold_touched");
        const int n = argc > 3 ? atoi(argv[3]) : 4;
        const int hold_ms = argc > 4 ? atoi(argv[4]) : 1500;
        std::vector<cl_mem> bufs;
        for (int i = 0; i < n; ++i) {
            cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
            CK(err);
            void* m = clEnqueueMapBuffer(q, b, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE,
                                         0, bytes, 0, nullptr, nullptr, &err);
            CK(err);
            if (touch) { memset(m, i + 1, bytes); clFinish(q); }
            bufs.push_back(b);
        }
        print_snap("held_peak", take_snapshot());
        printf("# holding %d x %zuMB (touched=%d) for %d ms; sampler should catch this\n",
               n, mb, (int)touch, hold_ms);
        fflush(stdout);
        usleep(hold_ms * 1000);
        for (cl_mem b : bufs) CK(clReleaseMemObject(b));
        clFinish(q);
        print_snap("after_release_all", take_snapshot());
    } else if (scenario == "create_nomap") {
        // Create buffers but NEVER clEnqueueMapBuffer them (pure GPU-side memory,
        // the common compute case). Optionally run a trivial GPU kernel would be
        // ideal, but even without it we can see whether an un-CPU-mapped buffer
        // lands in gpumem_unmapped vs gpumem_mapped.
        const int n = argc > 3 ? atoi(argv[3]) : 4;
        const int hold_ms = argc > 4 ? atoi(argv[4]) : 1200;
        std::vector<cl_mem> bufs;
        for (int i = 0; i < n; ++i) {
            cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE, bytes, nullptr, &err);
            CK(err);
            bufs.push_back(b);
        }
        print_snap("held_nomap", take_snapshot());
        printf("# %d x %zuMB created, never CPU-mapped, hold %d ms\n", n, mb, hold_ms);
        fflush(stdout);
        usleep(hold_ms * 1000);
        for (cl_mem b : bufs) CK(clReleaseMemObject(b));
        clFinish(q);
        print_snap("after_release_all", take_snapshot());
    } else if (scenario == "combo") {
        // Combined: malloc anon + hold a touched dma-buf + create+touch a GPU
        // buffer, then hold. Used to validate a disjoint smaps-free decomposition
        //   total = RssAnon + kernel + dma_held
        // against ground truth (each component known).
        const size_t anon_mb = 32, dma_mb = 64, gpu_mb = 128;
        void* anon = malloc(anon_mb * 1024 * 1024);
        memset(anon, 7, anon_mb * 1024 * 1024);
        int fd = dma_heap_alloc(dma_mb * 1024 * 1024);
        void* dh = fd >= 0 ? mmap(nullptr, dma_mb * 1024 * 1024, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0) : MAP_FAILED;
        if (dh != MAP_FAILED) memset(dh, 8, dma_mb * 1024 * 1024);
        cl_mem gpu = clCreateBuffer(ctx, CL_MEM_READ_WRITE, gpu_mb * 1024 * 1024,
                                    nullptr, &err);
        CK(err);
        void* gm = clEnqueueMapBuffer(q, gpu, CL_TRUE, CL_MAP_WRITE, 0,
                                      gpu_mb * 1024 * 1024, 0, nullptr, nullptr, &err);
        CK(err);
        memset(gm, 9, gpu_mb * 1024 * 1024);
        clFinish(q);
        printf("# combo: anon=%zuMB dma=%zuMB gpu=%zuMB (all touched)\n",
               anon_mb, dma_mb, gpu_mb);
        print_snap("combo_peak", take_snapshot());
        usleep((argc > 4 ? atoi(argv[4]) : 800) * 1000);
        clReleaseMemObject(gpu);
        free(anon);
        if (dh != MAP_FAILED) munmap(dh, dma_mb * 1024 * 1024);
        if (fd >= 0) close(fd);
        print_snap("after_release_all", take_snapshot());
    } else {
        fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
        return 1;
    }
    clReleaseCommandQueue(q);
    clReleaseContext(ctx);
    return 0;
}
