# Finding a memory leak

[中文版](MEMORY_LEAK.zh-CN.md) · [Usage](USAGE.md) · [README](../README.md)

A single report says what is live at one instant, which cannot tell a leak apart
from memory the workload legitimately holds. Two reports from the *same process*
can: run one full setup → work → teardown cycle several times, take a checkpoint
at the end of each, and whatever grew in between is what one cycle does not
release.

## 1. Put the checkpoint in the right place

Raise the hook's checkpoint signal from the workload itself, at the point where
resources should already have been released:

```c++
#include <signal.h>
#include <unistd.h>

void test() {
    create_ctx();
    use_ctx();
    free_ctx();
}

int main() {
    for (int i = 0; i < 2; ++i) {
        test();
        kill(getpid(), 33);   // 33 on Android; see the signal table below
    }
}
```

**Where you put this decides whether the result means anything.** It must go
where you believe the resources are already freed — after `free_ctx()`. A
checkpoint taken during `use_ctx()` samples the workload at its peak, and memory
that is still held at peak is not a leak, it is the workload working.

Two iterations is the minimum. More gives a staircase, which is stronger evidence
than a two-point difference: a real leak grows by roughly the same amount every
iteration, while one-off initialisation does not.

You do not need to link against anything for this. `kill(getpid(), sig)` is plain
libc, and the hook is attached with `LD_PRELOAD`, so the workload keeps building
and running exactly as before when the hook is absent.

## 2. Run with the hook attached

```sh
mkdir -p /data/local/tmp/trace
ALLOC_HOOK_DUMP_PREFIX=/data/local/tmp/trace/bt \
BACKTRACE_MIN_SIZE=1024 \
LD_LIBRARY_PATH=. LD_PRELOAD=liballoc_hook.so \
./your_workload
```

The checkpoint signal differs per platform:

| Platform | Default signal |
| --- | --- |
| Android (Bionic) | 33 (`BIONIC_SIGNAL_BACKTRACE`) |
| glibc Linux | `SIGRTMIN+6`, which is 40 on common builds |
| OHOS | 46 |

`BACKTRACE_DUMP_SIGNAL` overrides it. With `ALLOC_HOOK_DEBUG=1` the hook prints
the number it installed, which is the reliable way to check:

```
alloc_hook: installing dump signal 40
```

## 3. Read the reports

Reports are named
`<prefix>.<kind>.pid_<pid>.seq_<n>.time_<unix_seconds>.txt`, and `seq_` counts up
per process across every kind, so they sort into the order they were taken.

| Kind | What it is | Comparable with a checkpoint? |
| --- | --- | --- |
| `.signal.` | Live allocations at a checkpoint | yes |
| `.exit.` | The *peak* snapshot — the instant the process topped out | **no** |

Select by `pid_` first: `LD_PRELOAD` is inherited, so helper processes write
their own `seq_0`.

Entries are ordered by when each first appeared, so **compare from the back**:
the newest arrivals are what the later checkpoint added.

`tools/diff_checkpoint_reports.py` does the comparison, and `--iterations` turns
the totals into a per-iteration figure:

```sh
python3 tools/diff_checkpoint_reports.py \
    trace/bt.signal.pid_1234.seq_0.time_1788239358.txt \
    trace/bt.signal.pid_1234.seq_2.time_1788239358.txt \
    --iterations 2
```

```
== 1 stack(s) grew, +128.0 KB total (+64.0 KB per iteration) ==
   other stacks shrank +0.0 KB; net across all stacks +128.0 KB

+128.00KB  count 1 -> 3 (+2)  type=host  per_iter=+64.00KB/+1.0
#0 1a7b /path/to/your_workload
...
```

It refuses an `.exit.` report and refuses two reports from different processes.

### Interpreting it

- **Growth on a stack is a suspect, not a verdict.** A workload that reallocates
  the same buffers at different sizes shows large growth on some stacks and
  matching shrinkage on others. Check the `net across all stacks` line against
  `current host used` before calling anything a leak.
- **Allocations below `BACKTRACE_MIN_SIZE` carry no stack.** A leak made of many
  small blocks appears only as a rising `omitted_without_stack` count. Lower the
  threshold to bring them into the stack list — at the cost of capturing a stack
  for nearly every allocation.
- **`unattributed_anon` rising faster than `tracked_host`** means the growth is
  allocator retention or a mapping this build does not interpose. No allocation
  stack will explain it; the `rss_mapping` lines say which mapping grew.

## 4. What a checkpoint costs

The signal only takes a snapshot: one pass over the live allocation table,
aggregated by (stack, size), with no sort, no `/proc`, no formatting and no I/O.
Resolving module-relative PCs and writing the reports happen afterwards —
resolution on the hook's worker thread, report generation at shutdown, when the
process is on its way out anyway.

Measured on the host with 20k live tracked allocations: **~2 ms** for the
snapshot against ~400 ms to generate the report. Only the snapshot blocks the
workload's allocations. `test/checkpoint_cost_test.cpp` keeps it that way.

Two consequences worth knowing:

- **The snapshot is taken by a worker shortly after the signal, not inside it.**
  So the workload must be quiescent at the checkpoint — which is the same thing
  step 1 already asks for. In a loop whose iterations take microseconds the
  worker cannot keep up and several checkpoints collapse onto the same instant;
  at realistic iteration lengths (tens of milliseconds and up) each checkpoint
  lands where its signal was raised.
- **Checkpoint reports are written at shutdown.** Nothing appears on disk while
  the run is in progress. A process killed with `SIGKILL`, or one that calls
  `_exit`, loses them.

## 5. When you cannot change the workload

Send the signal from outside: `kill -<sig> <pid>`. The signal is only the
transport; what has to be decided outside is *when*, and the trigger should be
behaviour the workload already exhibits — an existing log line it prints once per
iteration, or a file it rewrites. No rebuild, and no `printf` added.

Two things measured the hard way on a real pipeline:

- **The line must be printed synchronously, after that iteration's teardown.** A
  line from a deferred logger can land after the *next* iteration has begun,
  putting the checkpoint in the middle of the work: one candidate line produced a
  250MB host total where the boundary was 9.7MB. A line printed before the
  iteration's `dlclose` is also too early.
- **Both checkpoints must sit at the same point of the cycle.** A trigger next to
  a large allocation can land on either side of it — the same trigger line once
  gave 12.7MB dma for one report and 214.8MB for the other, and the difference
  was entirely that allocation. The comparison tool warns when it sees such a
  step.

The last iteration has no boundary after it, and an external signal cannot outrun
process exit: by the time a poll notices the last teardown finished, the process
is often already gone. Have the workload run one extra throwaway iteration, so
the boundary you care about is followed by an observable one.

There is deliberately no "report what is live at exit" option to fall back on. It
would be captured from the hook's own destructor, which runs after every
application static destructor and atexit handler, so it only ever sees
allocations the program never freed at all: on a measured pipeline it read 0.54MB
against 9.70MB at a running boundary. A number that low reads as "no leak" and is
the wrong answer to the question being asked.

A debugger breakpoint that sends the signal is the most precise option and the
only one that reaches a code point with no observable side effect at all,
including a non-exported static function. It costs an `lldb-server`/`gdbserver`
on the target and a debuggable process.

## 6. Naming a report yourself

The exported C function writes a live report to a path you choose,
synchronously — when it returns, the file is there:

```c++
extern "C" void checkpoint(const char* file_name);

checkpoint("/data/local/tmp/trace/check_point.1.txt");
```

Link against `liballoc_hook.so`, or resolve it at runtime so the call is a no-op
when the hook is not preloaded:

```c++
typedef void (*checkpoint_func)(const char*);
auto checkpoint = (checkpoint_func)dlsym(RTLD_DEFAULT, "checkpoint");
if (checkpoint) {
    checkpoint("/data/local/tmp/trace/check_point.1.txt");
}
```

Unlike the signal, this generates the report immediately, so it pays the full
cost at the call site. Prefer the signal unless you need a specific filename.
