#include "AsyncStackPipeline.h"

#include <cassert>
#include <string>

int main() {
    const AsyncStackStats stats{
        .accepted = 2,
        .duplicates = 3,
        .dropped = 4,
        .processed = 5,
        .failed = 6,
        .worker_start_failures = 7,
        .recursive_submissions = 8,
        .queue_high_water = 9,
    };
    const std::string line = FormatAsyncStackStats(stats);
    assert(line == "accepted=2 duplicates=3 dropped=4 processed=5 failed=6 "
                   "worker_start_failures=7 recursive_submissions=8 "
                   "queue_high_water=9");
    return 0;
}
