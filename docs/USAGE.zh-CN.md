# 使用说明

[English usage](USAGE.md) · [中文架构](ARCHITECTURE.zh-CN.md) · [中文 README](../README.zh-CN.md)

## 1. 前置条件

- 主机构建需要 CMake 3.23 或更高版本以及 C++17 编译器。
- aarch64 Linux 构建需要 `aarch64-none-linux-gnu` 工具链，并通过
  `ARM_GNU_TOOLCHAIN_PATH` 提供。
- Android 构建需要匹配的 Android NDK，并通过 `NDK_ROOT` 提供。
- OHOS 构建需要 OpenHarmony native SDK，并通过 `OHOS_NDK_ROOT`
  （或 `NDK_ROOT`）提供。
- 目标进程必须是原生进程。Java/Kotlin 托管栈、ART/Dex、Ark/JSVM 或其他线程
  栈不属于核心契约。

主机构建明确支持 glibc Linux。OHOS 通过显式工具链身份选择，不会由普通 musl
构建自动推断。

## 2. 构建

### Linux

```sh
./build_linux.sh host                 # 本机构建，并跑测试套件
```

要给 aarch64 Linux 设备构建，把 `ARM_GNU_TOOLCHAIN_PATH` 指向
`aarch64-none-linux-gnu` 工具链：

```sh
export ARM_GNU_TOOLCHAIN_PATH=/path/to/gcc-arm-aarch64-none-linux-gnu
./build_linux.sh arm64
```

产物分别是 `out/linux-host/lib/liballoc_hook.so` 和
`out/linux-arm64/lib/liballoc_hook.so`。它们刻意不放在 `out/lib` —— Android 和
OHOS 两个脚本都装到那里，所以那里永远是最后一次构建的平台。把 Bionic 版 preload
到 glibc 目标上会报 `libm.so: cannot open shared object file`，因为 Bionic 的
soname 没有版本号。脚本会校验产物的机器类型和 libc soname，让这类错误在构建阶段就
暴露，而不是等目标机的 loader 抛出难懂的错误。

DMA 抓取默认开启，真机上应保持开启：流水线的大部分内存都是 DMA，关掉会让报告看起来
几乎是空的。只有在没有任何驱动 UAPI 的宿主 smoke 构建里才设
`ALLOC_HOOK_DMA_CAPTURE=OFF`。交叉构建默认不编测试（在本机跑不了）；设
`ALLOC_HOOK_BUILD_TESTS=ON` 可以得到能在设备上运行的测试程序，位于
`build_linux/test/`。

### Android

```sh
export NDK_ROOT=/path/to/android-ndk
./build_android.sh arm64-v8a
```

脚本对 arm64 和 armeabi-v7a 使用 API level 21，将库安装到 `out/lib`，
然后通过 `adb` 运行内置 smoke workload。请连接匹配的设备并确保
`adb` 在 `PATH` 中；如果只需要库文件，可直接使用 CMake 构建。

### OHOS

```sh
export OHOS_NDK_ROOT=/path/to/ohos-sdk/native
./build_ohos.sh arm64-v8a
```

`OHOS_ENABLE_MMAP_HOOK=ON` 是受控复现时的可选开关；`build_ohos.sh` 会将该
环境变量转发为 `MALLOC_HOOK_OHOS_MMAP_HOOK` CMake 选项。默认 OHOS 构建关闭
mmap 拦截。

## 3. 选择抓栈和采样

```sh
export ALLOC_HOOK_CAPTURE_MODE=fast       # fast 或 accurate
export ALLOC_HOOK_SAMPLING_INTERVAL_BYTES=4096
export BACKTRACE_MIN_SIZE=4096
```

默认是 Fast。若编译器运行时提供该能力，Fast 使用 `_Unwind_Backtrace` 有界地
获取原始 PC，然后异步解析模块和符号。`ALLOC_HOOK_SAMPLING_INTERVAL_BYTES`
只对 Fast 生效；`0` 和 `1` 表示关闭采样。Accurate 选择平台后端，并关闭 host
字节采样。

其他公共配置：

| 变量 | 作用 |
| --- | --- |
| `DUMP_PEAK_VALUE_MB` | 存活总量超过阈值后启用峰值快照。 |
| `DUMP_PEAK_STEP_MB` | 两次峰值快照之间所需的最小增长量。 |
| `BACKTRACE_DUMP_SIGNAL` | 覆盖平台默认的检查点信号。 |
| `ALLOC_HOOK_DEBUG` | 在 stderr 输出 hook 诊断（信号 worker、unwind、ION/DMA）。 |

采样只影响 host 分配归因；对通过平台导出策略和资源过滤的 mmap、DMA、ioctl
事件，资源记账仍然精确。

## 4. 预加载原生进程

```sh
mkdir -p ./trace
ALLOC_HOOK_CAPTURE_MODE=fast \
BACKTRACE_MIN_SIZE=4096 \
LD_LIBRARY_PATH="$PWD/out/linux-host/lib" \
LD_PRELOAD="$PWD/out/linux-host/lib/liballoc_hook.so" \
./your_program arg1 arg2
```

在 Android/OHOS 上，将库复制到目标设备的可写目录，并使用平台 shell 的预加载
方式。目标进程 ABI 必须与库一致。

拦截器覆盖导出的 C 分配函数族；OHOS 还会导出配置中的 C++ new/delete 族。
mmap 和资源 hook 受目标平台导出策略控制。直接系统调用和未导出的厂商入口会
绕过拦截器，这属于已知限制，不是抓栈失败。

## 5. 检查点和输出

有两种方式让 hook 输出当前存活的分配。

配置的信号只**抓一份快照**就返回，报告在退出时才写出。这样业务的分配被阻塞的时长
是"抓一份快照"（2 万条存活分配约 2 ms），而不是"生成一份报告"（约 400 ms）——
一个把进程卡住的检查点会扰动它自己正在测量的对象：

```sh
kill -<BACKTRACE_DUMP_SIGNAL> <pid>
```

导出的 C 函数 `checkpoint(const char*)` 则是**同步**把存活分配报告写入指定路径 ——
调用返回时文件就在那里 —— 因此全部成本都付在调用点。

信号触发的报告命名为

```
<ALLOC_HOOK_DUMP_PREFIX>.signal.pid_<pid>.seq_<n>.time_<unix 秒>.txt
```

正常退出写出的报告用同样的形式，只是把 `.signal.` 换成 `.exit.`（峰值快照）。
`seq_` 在同一进程内跨所有类型统一递增，因此报告能按采集顺序排序，同一秒内的两次
检查点也不会互相覆盖。序号是每进程独立的：全进程预加载时短生命周期的子进程也会写
自己的 `seq_0`，所以务必先按 `pid_` 筛选。

报告包含 host/资源总量、分配大小和类型、时间戳、抓栈状态/错误、解析状态，以及
模块快照和符号器可用时的符号化栈。正常退出会在报告前刷新尚未处理的唯一栈。
`_exit`、致命信号和 `SIGKILL` 无法保证正常 worker 刷新。

进程已经在退出，也不会丢掉它自己请求过的检查点：退出流程在封锁分配器操作之前，
会为已经请求的报告等待（最多 5 秒）。两条入口 —— 信号和 `checkpoint()` —— 走的是
同一套守卫，每一种拒绝都会明确打到 stderr，而不是静默丢弃：

| stderr 输出 | 含义 |
| --- | --- |
| `checkpoint ignored, the process is already exiting` | 请求到得太晚，退出流程已经过了还能写报告的时点。改成从业务内部发信号，它就会落在程序顺序里。 |
| `checkpoint ignored, the hook is not initialized` | 在 hook 起来之前调用，此时没有任何分配状态可报告。 |
| `checkpoint ignored, this thread is already inside the hook` | 从 hook 内部重入了。不要在分配器回调或 `pthread_atfork` prepare handler 里调 `checkpoint()`。 |
| `checkpoint ignored, no report path given` | `checkpoint()` 收到了空指针或空字符串路径。 |
| `checkpoint dropped, dump worker queue is full` | 信号来的速度超过了报告写出的速度。 |
| `cannot write report to <path>` | 前缀所在目录不可写。 |
| `gave up waiting for N checkpoint report(s)` | 等了 5 秒仍有报告未完成；选择继续退出而不是挂住进程。如果是报告的 fd 本身卡住（文件系统满或挂起），退出仍可能随后堵在它后面。 |

请正确理解这些数值：

- Fast 采样记录的是估算后的 host 记账大小，不是驻留内存。
- `DMA+RSS Max (sampling)` 包含驻留映射和运行时开销，其中部分不在 hook
  的存活分配表中。
- host 和 DMA 的分量峰值可能发生在不同时间；应使用 hook 自身时间一致的总峰值，
  不要直接相加两个独立最大值。
- `partial` 抓栈、模块无法解析、队列丢弃和符号器失败都会以显式状态输出，不会
  伪造栈帧。

### 定位每轮的泄漏

对比同一进程的两个检查点是定位每轮泄漏的手段。这套流程有独立的文档，包括检查点
必须打在业务的什么位置结果才有意义：

[`MEMORY_LEAK.zh-CN.md`](MEMORY_LEAK.zh-CN.md)

## 6. 故障排查

### 进程在库初始化前退出

检查 ABI、loader 搜索路径和目标目录写权限。libc 符号和 hook 状态准备好之前，
bootstrap 分配使用原始 mmap。

### Fast 报告符号很少或为空

当前 worker 侧路径会快照已加载模块范围，并通过 `dladdr` 获取动态符号名。
匹配的 debug 文件可供未来/自定义离线符号器使用，但不会让当前
`NativeSymbolizer` 自动具备完整 DWARF 解析能力。Fast 保存原始 PC，符号化
异步进行；对于 strip 后、已卸载或未导出的符号，结果可能保持 unresolved。
遇到复杂原生栈时，可以切换 Accurate。

### OHOS 没有 mmap 记录

OHOS 默认关闭 mmap 导出策略。只在受控复现中使用
`OHOS_ENABLE_MMAP_HOOK=ON` 重建。

### 出现队列或 worker 错误

检查报告中的解析状态和队列计数器。队列有界是为了避免无限阻塞分配路径；丢弃工作
比无限延迟一次分配更安全。可使用 Fast 采样降低捕获量，或提高检查点频率。

## 7. 已知限制

- 核心覆盖范围是原生 C/C++ 和当前线程抓栈。
- ART/Dex、Ark/JSVM、托管运行时帧、远程线程寄存器抓取和离线 unwind 输入，
  是未来后端的可选能力。
- ioctl 只记录调用方用户态栈，不代表内核或异步设备执行栈。
- 直接系统调用会绕过符号拦截。
- Accurate 后端是否可用以及栈质量取决于平台和工具链。
