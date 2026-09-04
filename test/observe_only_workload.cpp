// Workload for the observe-only runtime gate: a native process whose resident
// footprint rises and falls while the library is preloaded.
//
// Deliberately trivial and silent. What the gate checks is not this program's
// output but the hook's: which figures it prints, and whether it wrote a report
// file at all.
#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <vector>

int main() {
    // Long enough for several sampling cycles at the interval the gate asks for,
    // and touched page by page so the growth is resident rather than reserved.
    static constexpr size_t kChunkBytes = 4 * 1024 * 1024;
    static constexpr int kChunks = 8;
    std::vector<void*> chunks;
    for (int i = 0; i < kChunks; ++i) {
        void* chunk = malloc(kChunkBytes);
        if (chunk == nullptr) {
            return 1;
        }
        memset(chunk, i + 1, kChunkBytes);
        chunks.push_back(chunk);
        usleep(20 * 1000);
    }
    for (void* chunk : chunks) {
        free(chunk);
    }
    usleep(20 * 1000);
    return 0;
}
