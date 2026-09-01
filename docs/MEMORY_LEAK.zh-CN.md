# 内存泄漏排查

[English](MEMORY_LEAK.md) · [使用说明](USAGE.zh-CN.md) · [中文 README](../README.zh-CN.md)

单份报告只说明某一时刻有什么存活，无法区分泄漏和业务本来就要持有的内存。对比
**同一进程**的两份报告才可以：把一次完整的 初始化 → 处理 → 释放 循环跑若干次，
每轮结束时打一个检查点，中间长出来的就是一轮没有释放掉的东西。

## 1. 检查点要打在对的位置

在业务代码里、在资源本该已经释放的位置，自己触发 hook 的检查点信号：

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
        kill(getpid(), 33);   // Android 上是 33，其他平台见下面的信号表
    }
}
```

**打在哪里，决定了结果有没有意义。** 一定要打在你认为资源已经释放掉的位置 ——
也就是 `free_ctx()` 之后。打在 `use_ctx()` 阶段采到的是业务的用量高峰，而高峰时
还持有的内存不叫泄漏，那是业务正在干活。

两轮是最少的。多跑几轮会得到一个阶梯，比两点相减更有说服力：真泄漏每轮涨的量
大致相同，而一次性初始化不会。

这不需要链接任何东西。`kill(getpid(), sig)` 就是普通 libc 调用，hook 是靠
`LD_PRELOAD` 挂上去的，所以不挂 hook 时业务照原样编译运行。

## 2. 挂上 hook 运行

```sh
mkdir -p /data/local/tmp/trace
ALLOC_HOOK_DUMP_PREFIX=/data/local/tmp/trace/bt \
BACKTRACE_MIN_SIZE=1024 \
LD_LIBRARY_PATH=. LD_PRELOAD=liballoc_hook.so \
./your_workload
```

检查点信号各平台不同：

| 平台 | 默认信号 |
| --- | --- |
| Android（Bionic） | 33（`BIONIC_SIGNAL_BACKTRACE`） |
| glibc Linux | `SIGRTMIN+6`，常见构建上是 40 |
| OHOS | 46 |

`BACKTRACE_DUMP_SIGNAL` 可以覆盖。设 `ALLOC_HOOK_DEBUG=1` 时 hook 会打印它实际
装上的信号号，这是最可靠的确认方式：

```
alloc_hook: installing dump signal 40
```

## 3. 读报告

报告命名为
`<prefix>.<kind>.pid_<pid>.seq_<n>.time_<unix 秒>.txt`，`seq_` 在同一进程内跨所有
类型统一递增，因此报告能按采集顺序排序。

| 类型 | 是什么 | 能和检查点相减吗 |
| --- | --- | --- |
| `.signal.` | 某个检查点时刻的存活分配 | 能 |
| `.exit.` | **峰值**快照 —— 进程占用最高的那一刻 | **不能** |

务必先按 `pid_` 筛选：`LD_PRELOAD` 会被子进程继承，辅助进程也会写自己的 `seq_0`。

条目按各自最早出现的时间排序，所以**从后向前对比**：靠后的就是后一个检查点新增
的那些。

`tools/diff_checkpoint_reports.py` 负责对比，`--iterations` 把总量换算成每轮的量：

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

它会拒绝 `.exit.` 报告，也会拒绝来自不同进程的两份报告。

### 结果怎么读

- **某个栈涨了只是嫌疑，不是结论。** 如果业务用不同大小反复重新分配同一批
  buffer，就会出现一部分栈大涨、另一部分对应地下降。定性为泄漏之前，先用
  `net across all stacks` 这一行和 `current host used` 相互校验。
- **小于 `BACKTRACE_MIN_SIZE` 的分配不带栈。** 由大量小块组成的泄漏只会表现为
  `omitted_without_stack` 计数上涨。把阈值调低才能让它们进入栈列表 —— 代价是
  几乎每次分配都要抓栈。
- **`unattributed_anon` 涨得比 `tracked_host` 快**，说明增长来自分配器缓存或者
  本次构建没有拦截的映射，任何分配栈都解释不了它；`rss_mapping` 各行会指出是哪个
  映射在涨。

## 4. 一次检查点的成本

信号只做一件事：抓快照 —— 单趟遍历存活分配表，按 (栈, size) 聚合，不排序、不读
`/proc`、不格式化、不写盘。解模块相对 PC 和写报告都在之后做：解析在 hook 的
worker 线程上，报告生成放到进程退出时 —— 那时进程反正要走了。

host 上实测，2 万条存活追踪分配：抓快照 **约 2 ms**，而生成报告约 400 ms。只有
抓快照这一段会阻塞业务的分配。`test/checkpoint_cost_test.cpp` 负责把这个性质钉住。

两个需要知道的后果：

- **快照是 worker 在收到信号之后不久抓的，不是在信号处理函数里抓的。** 所以业务在
  检查点处必须是静止的 —— 这正是第 1 节已经要求的。如果一轮循环只有微秒级，worker
  跟不上，几个检查点会落到同一个时刻；在真实的轮长（几十毫秒以上）下，每个检查点都
  落在它的信号被发出的位置。
- **检查点报告是在退出时写的。** 运行期间磁盘上不会出现任何文件。被 `SIGKILL` 杀掉
  或者调用 `_exit` 的进程会丢掉它们。

## 5. 改不了业务代码时

从外面发信号：`kill -<sig> <pid>`。信号只是传输通道，需要在外面决定的是**什么时候**，
而这个触发点应该是业务本来就有的行为 —— 它每轮都会打的某行日志，或者它每轮重写的
某个文件。不需要重编，也不需要新加 `printf`。

在真实流水线上踩出来的两点：

- **必须是同步打出、且在该轮释放做完之后的那一行。** 来自延迟 logger 的行可能在
  **下一轮已经开始之后**才落到日志里，checkpoint 于是打在跑图中间：某个候选行给出的
  host 总量是 250MB，而真正的边界只有 9.7MB。打在该轮 `dlclose` 之前的行同样太早。
- **两个检查点必须处在周期的同一个位置。** 紧挨着大块分配的触发点可能落在它的两侧
  —— 实测中同一个触发行给出的 dma 一份是 12.7MB、另一份是 214.8MB，差值完全来自那
  一次分配。对比工具发现这种台阶时会告警。

最后一轮之后没有边界，而外部信号追不上进程退出：等轮询发现最后一轮拆解做完时，
进程往往已经没了。让业务多跑一轮丢弃的迭代，这样你关心的那个边界后面就有一个可
观测的边界。

这里刻意**没有**"输出退出时存活集"这种兜底选项。那种报告只能在 hook 自己的析构里
抓，而它跑在业务所有静态析构和 `atexit` 之后，所以只能看到程序从头到尾压根没释放
过的那部分：实测流水线上它读到 0.54MB，而运行中的边界是 9.70MB。这么低的数字会被
读成"没泄漏"，是对所问问题的错误回答。

用调试器断点发信号是最精确的办法，也是唯一能命中完全没有可观测副作用的代码点的办法
（包括没有导出的静态函数）。代价是目标上要有 `lldb-server`/`gdbserver`，且进程可调试。

## 6. 自己指定报告文件名

导出的 C 函数会把存活报告同步写到你指定的路径 —— 调用返回时文件就已经在那里：

```c++
extern "C" void checkpoint(const char* file_name);

checkpoint("/data/local/tmp/trace/check_point.1.txt");
```

链接 `liballoc_hook.so`，或者运行时解析，这样不挂 hook 时调用就是空操作：

```c++
typedef void (*checkpoint_func)(const char*);
auto checkpoint = (checkpoint_func)dlsym(RTLD_DEFAULT, "checkpoint");
if (checkpoint) {
    checkpoint("/data/local/tmp/trace/check_point.1.txt");
}
```

和信号不同，这个入口会**当场**生成报告，也就是在调用点付掉全部成本。除非你需要
指定文件名，否则优先用信号。
