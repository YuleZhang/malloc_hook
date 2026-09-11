# 获取 hook 报告

[English version / 英文版](get_hook_report.md) · [README](../README.zh-CN.md)

本库有四种模式，由设置了哪些环境变量完全决定。两种是*探测*，只测一个峰值并在退出时
打印；两种是*报告*，额外抓栈把峰值归因到调用点——开销就在抓栈这件事上。两种探测的区别
在于**测哪个量**：

- **tracked（跟踪量）**——走 `malloc`/`mmap`/`ioctl` 拦截路径记账的请求字节数，按 host /
  dma / total 汇总。
- **observed（实测量）**——进程真实占用，来自 `/proc`：在独立线程上采样的
  `VmRSS` + dmabuf + GPU 映射。

| 设置 | 模式 | 被插入的调用 | 产出 | 回答的问题 |
| --- | --- | --- | --- | --- |
| *(什么都不设)* — 裸 `LD_PRELOAD` | tracked 探测（轻量） | 跟踪、不抓栈 | 退出时 stderr 上一个 tracked 峰值盒子 | hook 跟踪到多少：host / dma / total 记账字节 |
| `ALLOC_HOOK_PEAK_SAMPLE_MS=k` | 只观测探测 | 原样转发给 libc | 退出时 stderr 上一个 observed 峰值盒子 | 进程占了多少：`/proc` 的 rss / dma / gpu |
| `DUMP_PEAK_VALUE_MB=N` | 首次越线 | 跟踪，整个运行一次栈遍历 | 一份**报告文件** | 判据首次超过 `N` MB 时是谁占着内存 |
| `ALLOC_HOOK_PEAK_SAMPLE_MS=k` + `DUMP_PEAK_STEP_MB=s` | 峰值追踪 | 跟踪，每涨 `s` 一次栈遍历 | 一份**报告文件** | 运行期最大值时刻是谁占着内存 |

两种探测回答"多少"；两种报告回答"哪些调用点"，并为此付费。除此之外没有任何东西会开启
报告：只给间隔、或只给步长，得到的是探测或什么都没有。

**报告用哪个判据是关键。** 有采样器时（`ALLOC_HOOK_PEAK_SAMPLE_MS>0`），
`DUMP_PEAK_VALUE_MB` 比较的是 *observed* 合计；采样器关闭时
（`ALLOC_HOOK_PEAK_SAMPLE_MS=0`），比较的是 *tracked* 合计——正是裸 tracked 探测打印的那个
`peak_tot`。所以两种 tracked 模式成对使用：先裸跑一次读出 tracked 合计，再把
`DUMP_PEAK_VALUE_MB` 设到略低于它、配 `ALLOC_HOOK_PEAK_SAMPLE_MS=0`，触发快照的判据就是你
刚测的那个量——报告不会"越了线却是空的"。

`0` 是各开关统一的关闭方式：`DUMP_PEAK_VALUE_MB=0` 不要首次越线快照，
`DUMP_PEAK_STEP_MB=0` 不要追踪，`ALLOC_HOOK_PEAK_SAMPLE_MS=0` 不起采样线程（判据退回
tracked 字节）。

两种报告模式都会在正常退出时写到 `ALLOC_HOOK_DUMP_PREFIX`，并在目录不存在时自动
创建。两个变量同时设置时按首次越线处理，采样间隔用你给的值。

## 0. 轻量 tracked 探测（裸 `LD_PRELOAD`）

不设任何 peak 变量时的默认模式。它保留存活分配表和 host/dma/total 峰值计数器，但
**不抓栈**——因此足够轻，可以顺手跑一遍看"hook 到底看到多少"：

```sh
LD_PRELOAD=/path/liballoc_hook.so ./your_program
```

```text
alloc_hook: ============================================================
alloc_hook:               Tracked Allocation Peak
alloc_hook: ------------------------------------------------------------
alloc_hook:   Tracked host peak (malloc/mmap):               363.55 MB
alloc_hook:   Tracked DMA peak (ioctl):                      144.79 MB
alloc_hook:   Tracked total peak:                            507.83 MB
alloc_hook:   RSS Max (getrusage):                           474.39 MB
alloc_hook: ============================================================
alloc_hook: tracked probe: no stacks captured; add DUMP_PEAK_VALUE_MB (with ALLOC_HOOK_PEAK_SAMPLE_MS=0) to attribute the peak to call sites
```

这些是**记账字节**，不是 `/proc` 的 RSS：`Tracked host` 是走 `malloc`/`mmap` 请求的量，
`Tracked DMA` 是走拦截的 `ioctl` 请求的量。它们可能超过 `RSS Max (getrusage)`（DMA buffer
和未驻留的分配计在这里、却不计入 RSS）；getrusage 那行是内核的真实驻留高水位，作为参照，
让你看出 tracked 字节覆盖了多少真实占用。

把 `Tracked total peak` 当作首次越线的下限：设 `DUMP_PEAK_VALUE_MB` 到略低于它、配
`ALLOC_HOOK_PEAK_SAMPLE_MS=0`（见 2a）。因为探测和该报告用的是同一个 `peak_tot`，跑一次
探测就够抓到贴近峰值的快照——报告里的 `snapshot_lag` 会告诉你下限还能再抬多少。

因为不抓栈，`checkpoint()`（导出函数或信号）写出的是**无栈**的存活报告——存活表加 tracked
峰值，没有调用点。需要栈就配报告模式。`fork` 子进程跑自己的空跟踪器、打印一个全零盒子；
通过 `_exit()` 或致命信号离开的进程什么都不打印。

## 1. 只观测探测模式

符号插入无法在运行时关闭——`LD_PRELOAD` 已经完成绑定——但被插入的调用**做不做事**
是可以的，而跟踪只有在报告会消费它时才划得来。所以只要求一个采样节奏的运行根本不会
构造跟踪器：

```sh
LD_PRELOAD=/path/liballoc_hook.so ALLOC_HOOK_PEAK_SAMPLE_MS=5 ./your_program
```

```text
alloc_hook: ============================================================
alloc_hook:                 Memory Usage Summary
alloc_hook: ------------------------------------------------------------
alloc_hook:   DMA Max (sampling):                              0.00 MB
alloc_hook:   RSS Max (sampling):                             61.27 MB
alloc_hook:   GPU mmap Max (sampling):                        12.05 MB
alloc_hook:   RSS Max (getrusage):                            61.54 MB
alloc_hook:   DMA+RSS+GPU mmap Max (sampling):                73.29 MB
alloc_hook:   not measured, so not a zero: dma
alloc_hook: ------------------------------------------------------------
alloc_hook:   Sampling Period:                                    1 ms
alloc_hook:   Achieved Period:                                22.28 ms
alloc_hook: ============================================================
```

这段日志沿用宿主框架为同样三项打印的摘要形状、列宽和黄色标识，便于在同一份日志里并排
阅读；颜色只在 stderr 是终端时才输出。开销是一个读 `/proc` 的采样线程，加上每次被插入
调用一次 relaxed 读——实测每对 `malloc`/`free` 68 ns，不预加载任何库时 60 ns，而没有尺寸
过滤的跟踪模式是 2762 ns。

四行需要注意。前三行是各项自己的最大值，它们不必同时见顶，所以它们的和不等于合计行；
合计行是**同一轮采样**内三者之和的最大值，也就是外部评估者当作进程峰值上报的那个量。
`RSS Max (getrusage)` 是内核自己的高水位，明显高于采样得到的 RSS 行就说明两次采样之间
出现过一次驻留峰值。`Achieved Period` 高于 `Sampling Period` 说明读 `/proc` 的耗时超过
了间隔，采样器为保证不超过半个核自行降频。而没有可用接口的那一项会在 `not measured`
行里被点名，而不是让读者把 0.00 当成测得为零。

用它来判断一个进程有没有内存问题、问题有多大，同时几乎不扰动它：它不抓任何栈，所以说
不出是哪个调用点造成的。它也没有存活分配表，因此 `checkpoint()` 会把同样这些数据写到
指定路径而不是堆报告，checkpoint 信号被忽略而不是放任它打死一个只是在被测量的进程。
`fork` 出的子进程不打印，通过 `_exit()` 或致命信号离开的进程什么都不打印——这和跟踪模式
的报告是同一个限制。当答案是"有，而且有这么多"时，再加 `DUMP_PEAK_STEP_MB` 或
`DUMP_PEAK_VALUE_MB` 去看它花在哪。

DMA 默认就在测：采样器无条件从 `/proc` 读取 dmabuf 统计（它不依赖构建开关
`MALLOC_HOOK_ENABLE_DMA_CAPTURE`，那个开关控制的是被跟踪的 `ioctl`/`close` 拦截）。
宿主 smoke 构建里 DMA 行是 `0.00 MB` 只说明这个进程没有 dmabuf；真机流水线绝大部分是
DMA。`gpu` 这一项及其 `/proc/self/smaps` 的坑见
[`GPU_MEMORY_ACCOUNTING.zh-CN.md`](GPU_MEMORY_ACCOUNTING.zh-CN.md)。

## 2. 报告模式

### 2a. `DUMP_PEAK_VALUE_MB` —— 首次越线（常用）

```sh
# 判据 = tracked 合计；与裸 tracked 探测的数字配对
export DUMP_PEAK_VALUE_MB=480        # 首次越过 480MB；整个运行一次栈遍历
export ALLOC_HOOK_PEAK_SAMPLE_MS=0   # 关采样器 -> 判据为 tracked 字节
export BACKTRACE_MIN_SIZE=1024
```

给正值的下限即选择首次越线：打开峰值记录，只保留一张快照——判据首次越过该 MB 数时的
那一张。采样器关闭时（`ALLOC_HOOK_PEAK_SAMPLE_MS=0`，当目标是把 tracked 字节归因到调用点
时推荐）判据是 *tracked* 合计；有采样器时判据是 `/proc` 的 *observed* 合计。整个运行只做
一次栈遍历，越线之后不会再有任何分配线程被快照阻塞，这对被测流水线本身对时序敏感的场景
很重要。

下限取自裸 tracked 探测的 `Tracked total peak`（设到略低）。代价是堆栈描述的是下限那一刻
而不是峰值时刻，所以下限要贴近峰值才能回答"峰值时刻是谁占着内存"。调参看 `snapshot_lag`：
它就是下限还能往上抬多少。

```text
peak_retention: first_crossing floor=200.000000MB (single snapshot; step unused)
snapshot_lag: observed=+117.800781MB (of 323.628906MB peak)
```

### 2b. `ALLOC_HOOK_PEAK_SAMPLE_MS` + `DUMP_PEAK_STEP_MB` —— 峰值追踪

```sh
export ALLOC_HOOK_PEAK_SAMPLE_MS=5   # 峰值追踪；最好与外部采样器保持一致
export DUMP_PEAK_STEP_MB=1           # 最小的有效步长：最贴近最大值，栈遍历最多
export BACKTRACE_MIN_SIZE=1024       # 只有确实需要每个小分配的栈时才设为 0
```

正的步长**与间隔一起**才选择峰值追踪；单独设置不起任何作用。`DUMP_PEAK_STEP_MB` 是
重新构建峰值快照所需增长量的上限：步长越小快照越贴近最大值、栈遍历越多，峰值较小时实际
使用 25% 的增长量、下限 64 KB。

峰值追踪不需要事先知道峰值，首次运行就能拿到正确的峰值快照，代价是峰值每涨过一个步长
就要做一次栈遍历。

## 3. 采样节奏

常见场景下 `ALLOC_HOOK_PEAK_SAMPLE_MS` 不需要显式赋值。采样本进程内存的宿主框架
会把自己使用的间隔写在一个名字以 `AUTO_SHOW_MEM_USE_DURATION_MS` 结尾的环境变量
里；hook 发现它被设为正值时就直接沿用该间隔，这样快照时刻就落在该框架报出峰值的
同一瞬间，也不需要人工同步采样节奏。没有这个变量时，开启峰值记录后按 50ms 采样。
显式设置 `ALLOC_HOOK_PEAK_SAMPLE_MS` 会覆盖以上两者，包括设为 `0`——那表示完全不
起采样线程，改用跟踪到的分配字节数与下限比较；这是另一个量，报告会如实标注。

框架的那个变量只提供节奏，永远不会单独打开峰值记录：一个什么都没设的进程，不应该
因为环境里有它就凭空多出一个采样线程和一份退出报告。

这里不会读取历史累计字段 `VmPeak` 或 `VmHWM`，因为它们无法告诉 hook 应在哪一刻
复制存活堆栈。每一轮采样先读取当前 `VmRSS`，再读取 DMA 和 GPU 内存，并用同一轮
三者之和与此前最大值比较。当总和还越过 `DUMP_PEAK_VALUE_MB` 和
`DUMP_PEAK_STEP_MB` 的门槛时，回调会立即再次读取
`VmRSS`/`RssAnon`/`RssFile`/`RssShmem`，从 `/proc/self/smaps` 收集驻留量最高的
映射，并复制存活堆栈表。这些读取和堆栈复制是顺序执行的，不是内核提供的原子快照；
报告中的 `at_peak` 表示它们来自同一个峰值回调窗口。

## 4. 报告字段解读

报告会写明保留下来的快照描述的是哪一次越线、由哪种判据产生；如果是实测占用，还会
写明快照那一刻的实测值、整个 run 的最大值，以及采样器本身的开销：

```text
peak_retention: chase_max (snapshot refreshed per step)
peak_criterion: observed_host_rss_plus_dma_plus_gpu (from /proc, aligned with an external sampler)
observed_peak(at_snapshot):     rss=291.75MB dma=935.12MB gpu=24.00MB total=1250.87MB
observed_peak(max_of_sum):      rss=291.75MB dma=935.12MB gpu=24.00MB total=1250.87MB (...)
observed_peak(independent_max): rss=369.54MB dma=935.12MB gpu=24.00MB
observed_sampler: interval_ms=1 achieved_ms=1.58 dma_source=fd+maps gpu_source=smaps samples=9516 ...
```

`at_snapshot` 和 `max_of_sum` 相等就是目标状态：堆栈是在最大值那一刻抓的，而不是在
爬升过程中的某一级。首次越线模式下两者按设计就不相等，差值由 `snapshot_lag` 给出。

如果整个运行都没越过下限，就没有任何快照。此时报告不会输出一段空的堆栈——那看起来
和"hook 什么都没抓到"一样——而是退回列出报告时刻的存活分配，并写明原因：

```text
peak_snapshot: none (criterion never passed the floor; the list above is live at report time)
peak_criterion: none (nothing was snapshotted)
```

`achieved_ms` 大于请求的间隔说明读 `/proc` 的耗时超过了间隔，采样器自行降频以保证不
超过半个核——它不会谎报一个没达到的节奏。当 host 和设备内存在不同时刻见顶时，
`independent_max` 会高于 `max_of_sum` 中的任一项，而这正是这套机制存在的理由。

在存在受支持 GPU 设备节点的平台上，实测判据是 `rss + dma + gpu`；当前没有只排除这项未被
其他统计覆盖的 GPU 内存、强制改为 `rss + dma` 的运行时开关。`gpu` 这一项统计什么、为什么
会同时躲开 `rss` 和 `dma`、以及读它时 `/proc/self/smaps` 的代价，都在
[`GPU_MEMORY_ACCOUNTING.zh-CN.md`](GPU_MEMORY_ACCOUNTING.zh-CN.md) 里说明。

## 5. 所有模式共同的限制

采样器读的是 `/proc`，只能看到采样时刻的进程状态，持续时间不足一个间隔的峰值它抓不到——
被对齐的那个外部采样器同样抓不到。`dma_source=none` 表示这个内核没有可读取的 dmabuf
统计接口，不等于进程没有占用设备内存。而在没有这一遍所统计的设备节点的平台上
`gpu_source` 为 `not_applicable`，这同样不是"测得为零"，而是根本没去测。

报告在正常退出或收到 checkpoint 信号时写出。`_exit()`、致命信号和 `SIGKILL` 无法保证
正常的 worker 刷新。
