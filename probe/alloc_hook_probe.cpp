// A standalone, opt-in device memory probe. Never requires LD_PRELOAD.
#include "ion_uapi.h"
#include "dma_heap_uapi.h"
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr size_t kDefaultBytes = 16u * 1024u * 1024u;
constexpr size_t kMaximumBytes = 64u * 1024u * 1024u;
constexpr size_t kMaxEvidence = 8192;
FILE* output = stdout;

std::string Escape(const std::string& src) {
    std::string out;
    const char hex[] = "0123456789abcdef";
    for (unsigned char c : src) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else out += static_cast<char>(c);
    }
    return out;
}
class Json {
public:
    Json() { s_ << '{'; }
    void Text(const char* key, const std::string& value) {
        Key(key); s_ << '"' << Escape(value) << '"';
    }
    void Number(const char* key, int64_t value) {
        Key(key); s_ << value;
    }
    void Flag(const char* key, bool value) {
        Key(key); s_ << (value ? "true" : "false");
    }
    void Write() {
        s_ << "}\n";
        fputs(s_.str().c_str(), output);
        fflush(output);
    }
private:
    void Key(const char* key) {
        if (count_++) s_ << ',';
        s_ << '"' << key << "\":";
    }
    std::ostringstream s_;
    int count_ = 0;
};

struct ReadResult {
    std::string text;
    int error = 0;
    bool truncated = false;
};
ReadResult ReadFile(const std::string& path) {
    ReadResult r;
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) { r.error = errno; return r; }
    char buf[1024];
    while (r.text.size() < kMaxEvidence) {
        size_t nbytes = std::min(sizeof(buf), kMaxEvidence - r.text.size());
        ssize_t n = read(fd, buf, nbytes);
        if (n == -1 && errno == EINTR) continue;
        if (n < 0) { r.error = errno; break; }
        if (n == 0) break;
        r.text.append(buf, static_cast<size_t>(n));
    }
    if (r.text.size() == kMaxEvidence) {
        char extra;
        r.truncated = read(fd, &extra, 1) > 0;
    }
    close(fd);
    return r;
}
std::string FdField(const std::string& input, const std::string& key) {
    const std::string prefix = key + ':';
    size_t pos = 0;
    while (pos < input.size()) {
        size_t end = input.find('\n', pos);
        if (end == std::string::npos) end = input.size();
        if (input.compare(pos, prefix.size(), prefix) == 0) {
            pos += prefix.size();
            while (pos < end && (input[pos] == ' ' || input[pos] == '\t')) ++pos;
            return input.substr(pos, end - pos);
        }
        pos = end + 1;
    }
    return "";
}
uint64_t ClockNs() {
    timespec t = {};
    return clock_gettime(CLOCK_MONOTONIC, &t) == 0 ?
        uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec) : 0;
}
struct Options {
    std::string command = "inspect", backend = "auto", heap = "system";
    std::string cases, oracle, outfile;
    size_t bytes = kDefaultBytes;
    uint32_t mask = 0;
};
void Capability(const char* name, const std::string& path, bool directory = false) {
    struct stat st = {};
    bool exists = stat(path.c_str(), &st) == 0;
    bool type = exists && (!directory || S_ISDIR(st.st_mode));
    Json j;
    j.Text("kind", "capability");
    j.Text("name", name);
    j.Text("path", path);
    j.Flag("exists", exists);
    j.Flag("readable", type && access(path.c_str(), R_OK) == 0);
    j.Write();
}
std::vector<std::string> Heaps() {
    std::vector<std::string> names;
    DIR* dir = opendir("/dev/dma_heap");
    if (!dir) return names;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        std::string path = std::string("/dev/dma_heap/") + e->d_name;
        struct stat st = {};
        if (stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode))
            names.emplace_back(e->d_name);
    }
    closedir(dir);
    return names;
}
void Inspect() {
    struct utsname u = {};
    uname(&u);
    Json p;
    p.Text("kind", "platform");
    p.Text("sysname", u.sysname);
    p.Text("release", u.release);
    p.Text("machine", u.machine);
    p.Number("uid", getuid());
    p.Number("page_size", sysconf(_SC_PAGESIZE));
#ifdef __ANDROID__
    char sdk[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.version.sdk", sdk);
    p.Text("target", "android");
    p.Text("android_sdk", sdk);
#else
#ifdef ALLOC_PROBE_TARGET_OS
    p.Text("target", ALLOC_PROBE_TARGET_OS);
#else
    p.Text("target", "linux_or_ohos");
#endif
#endif
    p.Write();
    Json abi;
    abi.Text("kind", "compiled_abi");
    abi.Number("legacy_ion_alloc", ALLOC_HOOK_ION_IOC_ALLOC);
    abi.Number("modern_ion_alloc", ALLOC_HOOK_ION_IOC_NEW_ALLOC);
    abi.Number("dma_heap_alloc", DMA_HEAP_IOCTL_ALLOC);
    abi.Number("legacy_ion_struct_size", sizeof(ion_allocation_data));
    abi.Flag("runtime_abi_verified", false);
    abi.Write();
    Capability("ion", "/dev/ion");
    Capability("dma_heap", "/dev/dma_heap", true);
    Capability("proc_fdinfo", "/proc/self/fdinfo", true);
    Capability("dma_buf_debugfs", "/sys/kernel/debug/dma_buf/bufinfo");
    Capability("dma_buf_sysfs", "/sys/kernel/dmabuf/buffers", true);
    Capability("ion_debugfs", "/sys/kernel/debug/ion/heaps", true);
    Capability("selinux", "/sys/fs/selinux/enforce");
    for (const auto& h : Heaps()) Capability("dma_heap_node", "/dev/dma_heap/" + h);
    Json limitation;
    limitation.Text("kind", "limitation");
    limitation.Text("message", "dma-buf inode identifies an export, not necessarily a backing allocation; missing kernel counters are unknown, not zero");
    limitation.Write();
}
void Sample(const Options& o, const char* id, const char* stage,
            int rc, int error, int fd = -1, int handle = -1) {
    int saved = errno;
    Json j;
    j.Text("kind", "sample");
    j.Text("case", id);
    j.Text("stage", stage);
    j.Number("monotonic_ns", ClockNs());
    j.Number("result", rc);
    j.Number("errno", error);
    j.Number("dma_fd", fd);
    j.Number("ion_handle", handle);
    if (fd >= 0) {
        ReadResult r = ReadFile("/proc/self/fdinfo/" + std::to_string(fd));
        j.Flag("fdinfo_readable", r.error == 0);
        if (!r.error) {
            j.Text("fdinfo_raw", r.text);
            j.Text("fdinfo_ino", FdField(r.text, "ino"));
            j.Text("fdinfo_size", FdField(r.text, "size"));
            j.Text("fdinfo_count", FdField(r.text, "count"));
            j.Text("fdinfo_exp_name", FdField(r.text, "exp_name"));
            j.Flag("fdinfo_truncated", r.truncated);
        } else j.Number("fdinfo_errno", r.error);
    }
    if (!o.oracle.empty()) {
        ReadResult r = ReadFile(o.oracle);
        j.Text("oracle_path", o.oracle);
        j.Flag("oracle_readable", r.error == 0);
        if (!r.error) {
            j.Text("oracle_raw", r.text);
            j.Flag("oracle_truncated", r.truncated);
        } else j.Number("oracle_errno", r.error);
    }
    j.Write();
    errno = saved;
}
void Finished(const char* id, const char* status, const std::string& reason) {
    Json j;
    j.Text("kind", "case_end");
    j.Text("case", id);
    j.Text("status", status);
    j.Text("reason", reason);
    j.Flag("backing_allocation_independently_verified", false);
    j.Write();
}
bool Unsupported(int error) {
    return error == ENOTTY || error == ENOSYS || error == ENODEV ||
        error == EACCES || error == EPERM || error == EOPNOTSUPP;
}
struct Stats { int passed = 0, failed = 0, skipped = 0; };
void Failure(Stats* s, const char* id, const char* what, int error) {
    bool skip = Unsupported(error);
    if (skip) ++s->skipped; else ++s->failed;
    Finished(id, skip ? "unsupported" : "fail",
             std::string(what) + ": " + std::strerror(error));
}

struct Legacy {
    int client = -1;
    std::vector<int> fds, handles;
    void* mapping = MAP_FAILED;
    size_t mapped_bytes = 0;
    ~Legacy() { Cleanup(); }
    void Cleanup() {
        if (mapping != MAP_FAILED) {
            munmap(mapping, mapped_bytes);
            mapping = MAP_FAILED;
        }
        for (int fd : fds) close(fd);
        fds.clear();
        if (client >= 0) {
            for (int h : handles) {
                ion_handle_data a = {};
                a.handle = h;
                ioctl(client, ALLOC_HOOK_ION_IOC_FREE, &a);
            }
            close(client);
            client = -1;
        }
        handles.clear();
    }
    void UntrackFd(int fd) {
        auto it = std::find(fds.begin(), fds.end(), fd);
        if (it != fds.end()) fds.erase(it);
    }
    void UntrackHandle(int h) {
        auto it = std::find(handles.begin(), handles.end(), h);
        if (it != handles.end()) handles.erase(it);
    }
};
void LegacyCase(const Options& o, char letter, Stats* stats) {
    const char id[] = {letter, 0};
    Legacy state;
    Sample(o, id, "baseline", 0, 0);
    state.client = open("/dev/ion", O_RDWR | O_CLOEXEC);
    if (state.client < 0) {
        ++stats->skipped;
        Finished(id, "skip", std::string("open /dev/ion: ") + std::strerror(errno));
        return;
    }
    ion_allocation_data alloc = {};
    alloc.len = o.bytes;
    alloc.heap_id_mask = o.mask;
    errno = 0;
    int rc = ioctl(state.client, ALLOC_HOOK_ION_IOC_ALLOC, &alloc);
    int error = rc == 0 ? 0 : errno;
    Sample(o, id, "ion_alloc", rc, error, -1, rc == 0 ? alloc.handle : -1);
    if (rc < 0) { Failure(stats, id, "ION_IOC_ALLOC", error); return; }
    state.handles.push_back(alloc.handle);
    auto share = [&](int h) {
        ion_fd_data data = {};
        data.handle = h;
        errno = 0;
        int ret = ioctl(state.client, ALLOC_HOOK_ION_IOC_SHARE, &data);
        int err = ret == 0 ? 0 : errno;
        Sample(o, id, "ion_share", ret, err, ret == 0 ? data.fd : -1, h);
        if (ret == 0 && data.fd >= 0) state.fds.push_back(data.fd);
        return ret == 0 ? data.fd : -1;
    };
    auto release = [&](int h) {
        ion_handle_data data = {};
        data.handle = h;
        errno = 0;
        int ret = ioctl(state.client, ALLOC_HOOK_ION_IOC_FREE, &data);
        int err = ret == 0 ? 0 : errno;
        Sample(o, id, "ion_free", ret, err, -1, h);
        if (ret == 0) state.UntrackHandle(h);
        return ret == 0;
    };
    auto close_fd = [&](int fd) {
        errno = 0;
        int ret = close(fd);
        int err = ret == 0 ? 0 : errno;
        Sample(o, id, "close_dmabuf_fd", ret, err);
        if (ret == 0) state.UntrackFd(fd);
        return ret == 0;
    };
    int fd = share(alloc.handle);
    if (fd < 0) { Failure(stats, id, "ION_IOC_SHARE", errno ? errno : EIO); return; }
    bool ok = true;
    if (letter == 'B') {
        int second = share(alloc.handle);
        if (second < 0) { Failure(stats, id, "second SHARE", errno ? errno : EIO); return; }
        ok = release(alloc.handle);
        ok = close_fd(fd) && ok;
        ok = close_fd(second) && ok;
    } else if (letter == 'C') {
        ion_fd_data imported = {};
        imported.fd = fd;
        errno = 0;
        int ret = ioctl(state.client, ALLOC_HOOK_ION_IOC_IMPORT, &imported);
        int err = ret == 0 ? 0 : errno;
        Sample(o, id, "ion_import", ret, err, fd, ret == 0 ? imported.handle : -1);
        if (ret < 0) { Failure(stats, id, "ION_IOC_IMPORT", err); return; }
        state.handles.push_back(imported.handle);
        int second = share(imported.handle);
        if (second < 0) { Failure(stats, id, "SHARE imported handle", errno ? errno : EIO); return; }
        ok = release(imported.handle);
        ok = release(alloc.handle) && ok;
        ok = close_fd(fd) && ok;
        ok = close_fd(second) && ok;
    } else if (letter == 'D') {
        ok = close_fd(fd);
        ok = release(alloc.handle) && ok;
    } else if (letter == 'E') {
        errno = 0;
        state.mapping = mmap(nullptr, o.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int err = state.mapping == MAP_FAILED ? errno : 0;
        Sample(o, id, "mmap", state.mapping == MAP_FAILED ? -1 : 0, err, fd);
        if (state.mapping == MAP_FAILED) { Failure(stats, id, "mmap", err); return; }
        state.mapped_bytes = o.bytes;
        ok = close_fd(fd);
        errno = 0;
        int ret = munmap(state.mapping, state.mapped_bytes);
        int unmap_error = ret == 0 ? 0 : errno;
        Sample(o, id, "munmap", ret, unmap_error);
        if (ret == 0) state.mapping = MAP_FAILED;
        ok = (ret == 0) && ok;
        ok = release(alloc.handle) && ok;
    } else if (letter == 'F') {
        errno = 0;
        int ret = close(state.client);
        int err = ret == 0 ? 0 : errno;
        Sample(o, id, "close_ion_client", ret, err, fd);
        if (ret == 0) { state.client = -1; state.handles.clear(); }
        ok = ret == 0;
        ok = close_fd(fd) && ok;
    } else {
        ok = release(alloc.handle);
        ok = close_fd(fd) && ok;
    }
    state.Cleanup();
    Sample(o, id, "after_cleanup", 0, 0);
    if (ok) { ++stats->passed; Finished(id, "pass", "UAPI sequence completed; verify backing separately"); }
    else { ++stats->failed; Finished(id, "fail", "one or more UAPI operations failed"); }
}
void DirectCase(const Options& o, const std::string& backend, bool mapping, Stats* s) {
    const char* id = mapping ? "mapping" : "direct";
    const bool heap = backend == "dma-heap";
    const std::string path = heap ? "/dev/dma_heap/" + o.heap : "/dev/ion";
    Sample(o, id, "baseline", 0, 0);
    int allocator = open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (allocator < 0) {
        ++s->skipped;
        Finished(id, "skip", std::string("open allocator: ") + std::strerror(errno));
        return;
    }
    int fd = -1, rc = -1;
    errno = 0;
    if (heap) {
        dma_heap_allocation_data a = {};
        a.len = o.bytes;
        a.fd_flags = O_RDWR | O_CLOEXEC;
        rc = ioctl(allocator, DMA_HEAP_IOCTL_ALLOC, &a);
        if (rc == 0) fd = static_cast<int>(a.fd);
    } else {
        ion_new_allocation_data a = {};
        a.len = o.bytes;
        a.heap_id_mask = o.mask;
        rc = ioctl(allocator, ALLOC_HOOK_ION_IOC_NEW_ALLOC, &a);
        if (rc == 0) fd = static_cast<int>(a.fd);
    }
    int error = rc == 0 ? 0 : errno;
    Sample(o, id, "alloc_fd", rc, error, fd);
    if (rc < 0 || fd < 0) {
        Failure(s, id, "allocator ioctl", error ? error : EIO);
        close(allocator);
        return;
    }
    bool ok = true;
    void* mapped = MAP_FAILED;
    if (mapping) {
        errno = 0;
        mapped = mmap(nullptr, o.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        int err = mapped == MAP_FAILED ? errno : 0;
        Sample(o, id, "mmap", mapped == MAP_FAILED ? -1 : 0, err, fd);
        if (mapped == MAP_FAILED) ok = false;
    }
    errno = 0;
    rc = close(fd);
    Sample(o, id, "close_dmabuf_fd", rc, rc == 0 ? 0 : errno);
    ok = (rc == 0) && ok;
    if (mapped != MAP_FAILED) {
        errno = 0;
        int ret = munmap(mapped, o.bytes);
        Sample(o, id, "munmap", ret, ret == 0 ? 0 : errno);
        ok = (ret == 0) && ok;
    }
    close(allocator);
    Sample(o, id, "after_cleanup", 0, 0);
    if (ok) { ++s->passed; Finished(id, "pass", "UAPI sequence completed; verify backing separately"); }
    else { ++s->failed; Finished(id, "fail", "one or more UAPI operations failed"); }
}
bool Integer(const char* text, uint64_t* result) {
    if (!*text || *text == '-') return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || *end) return false;
    *result = value;
    return true;
}
void Usage(FILE* stream) {
    fprintf(stream,
        "alloc_hook_probe [inspect] [--output PATH]\n"
        "alloc_hook_probe run --backend auto|dma-heap|legacy-ion|modern-ion [options]\n"
        "  --heap NAME          dma-heap node (default: system)\n"
        "  --ion-heap-mask N    mandatory for ION; decimal or 0xhex\n"
        "  --bytes N            default 16777216, maximum 67108864\n"
        "  --cases A,B,C,D,E,F legacy ION sequences (default: all)\n"
        "  --oracle FILE        read-only kernel evidence snapshot per step\n"
        "  --output FILE        JSONL result file (default stdout)\n"
        "inspect is always read-only; run is explicit and allocates memory.\n");
}
int Run(int argc, char** argv) {
    Options o;
    if (argc > 1 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        Usage(stdout); return 0;
    }
    int i = 1;
    if (i < argc && argv[i][0] != '-') o.command = argv[i++];
    if (o.command != "inspect" && o.command != "run") { Usage(stderr); return 2; }
    while (i < argc) {
        const std::string key = argv[i++];
        if (key == "--help") { Usage(stdout); return 0; }
        if (i >= argc) { Usage(stderr); return 2; }
        const std::string value = argv[i++];
        if (key == "--backend") o.backend = value;
        else if (key == "--heap") o.heap = value;
        else if (key == "--cases") o.cases = value;
        else if (key == "--oracle") o.oracle = value;
        else if (key == "--output") o.outfile = value;
        else if (key == "--bytes" || key == "--ion-heap-mask") {
            uint64_t n = 0;
            if (!Integer(value.c_str(), &n)) { Usage(stderr); return 2; }
            if (key == "--bytes") {
                if (n == 0 || n > kMaximumBytes) return 2;
                o.bytes = static_cast<size_t>(n);
            } else {
                if (n == 0 || n > std::numeric_limits<uint32_t>::max()) return 2;
                o.mask = static_cast<uint32_t>(n);
            }
        } else { Usage(stderr); return 2; }
    }
    if (o.heap.empty() || o.heap == ".." || o.heap.find('/') != std::string::npos) return 2;
    if (o.backend != "auto" && o.backend != "dma-heap" &&
        o.backend != "legacy-ion" && o.backend != "modern-ion") return 2;
    if (o.command == "run" &&
        (o.backend == "legacy-ion" || o.backend == "modern-ion") && o.mask == 0) return 2;
    for (char ch : o.cases) if (ch != ',' && (ch < 'A' || ch > 'F')) return 2;
    if (!o.outfile.empty()) {
        output = fopen(o.outfile.c_str(), "w");
        if (!output) { perror("--output"); return 2; }
    }
    Inspect();
    if (o.command == "inspect") return 0;
    std::string backend = o.backend;
    if (backend == "auto") {
        std::vector<std::string> heaps = Heaps();
        if (std::find(heaps.begin(), heaps.end(), o.heap) != heaps.end()) backend = "dma-heap";
        else if (o.mask) backend = "legacy-ion";
        else backend = "unavailable";
    }
    Json config;
    config.Text("kind", "run_config");
    config.Text("backend", backend);
    config.Number("requested_bytes", o.bytes);
    config.Text("heap", o.heap);
    config.Flag("kernel_oracle_configured", !o.oracle.empty());
    config.Write();
    if (backend == "unavailable") {
        Finished("discovery", "unsupported", "no selected dma-heap; specify an ION backend and heap mask");
        return 3;
    }
    Stats stats;
    if (backend == "legacy-ion") {
        std::string cases = o.cases.empty() ? "ABCDEF" : o.cases;
        std::set<char> seen;
        for (char letter : cases)
            if (letter != ',' && seen.insert(letter).second) LegacyCase(o, letter, &stats);
    } else {
        if (!o.cases.empty()) { Usage(stderr); return 2; }
        DirectCase(o, backend, false, &stats);
        DirectCase(o, backend, true, &stats);
    }
    Json summary;
    summary.Text("kind", "summary");
    summary.Number("passed", stats.passed);
    summary.Number("failed", stats.failed);
    summary.Number("unsupported_or_skipped", stats.skipped);
    summary.Flag("kernel_backing_lifetime_proven", false);
    summary.Write();
    return stats.failed ? 1 : (stats.passed ? 0 : 3);
}
} // namespace

int main(int argc, char** argv) { return Run(argc, argv); }
