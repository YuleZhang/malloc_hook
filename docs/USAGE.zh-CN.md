# 使用说明

[English usage](USAGE.md) · [中文架构](ARCHITECTURE.zh-CN.md) · [中文 README](../README.zh-CN.md)

## 1. 前置条件

- 主机构建需要 CMake 3.23 或更高版本以及 C++17 编译器。
- Android 构建需要匹配的 Android NDK，并通过 `NDK_ROOT` 提供。
- OHOS 构建需要 OpenHarmony native SDK，并通过 `OHOS_NDK_ROOT`
  （或 `NDK_ROOT`）提供。
- 目标进程必须是原生进程。Java/Kotlin 托管栈、ART/Dex、Ark/JSVM 或其他线程
  栈不属于核心契约。

主机构建明确支持 glibc Linux。OHOS 通过显式工具链身份选择，不会由普通 musl
构建自动推断。

## 2. 构建

### 主机 Linux

```sh
./build_linux.sh host
```

该脚本执行本机构建、运行测试、把库安装到 `out/linux-host/lib`，并打印实际生效的
构建选项。等价的手工构建方式为：

```sh
cmake -S . -B build-host \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-host --target alloc_hook
ctest --test-dir build-host --output-on-failure
cmake --install build-host --prefix "$PWD"
```

生成的库为 `out/lib/liballoc_hook.so`。

要交叉编译 glibc Linux/aarch64，请将 `ARM_GNU_TOOLCHAIN_PATH` 指向
`aarch64-none-linux-gnu` 工具链根目录，然后运行 `./build_linux.sh arm64`。

### Android

```sh
export NDK_ROOT=/path/to/android-ndk
./build_android.sh arm64-v8a
```

脚本对 arm64 和 armeabi-v7a 使用 API level 21，将库安装到 `out/lib`，
然后通过 `adb` 运行内置 smoke workload。请连接匹配的设备并确保
`adb` 在 `PATH` 中；如果只需要库文件，可直接使用 CMake 构建。
成功编译后，脚本会打印实际生效的 CMake 选项和派生出的导出策略。

### OHOS

```sh
export OHOS_NDK_ROOT=/path/to/ohos-sdk/native
./build_ohos.sh arm64-v8a
```

`OHOS_ENABLE_MMAP_HOOK=ON` 是受控复现时的可选开关；`build_ohos.sh` 会将该
环境变量转发为 `MALLOC_HOOK_OHOS_MMAP_HOOK` CMake 选项。默认 OHOS 构建关闭
mmap 拦截。
成功编译后，脚本会打印实际生效的 CMake 选项和派生出的导出策略。

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
| `DUMP_PEAK_VALUE_MB` | 启用峰值记录、退出时导出，并在当前所选峰值判据超过该下限后开始抓取快照。 |
| `DUMP_PEAK_STEP_MB` | 再次抓取峰值快照所需增长量的上限；小峰值使用 25% 增长量，下限为 64 KB。`0` 表示每次新峰值都抓。 |
| `ALLOC_HOOK_PEAK_SAMPLE_MS` | 按该正整数毫秒间隔采样实测 `VmRSS + DMA + GPU` 总量，并将它作为峰值判据。需要同时设置 `DUMP_PEAK_VALUE_MB`；`0` 表示强制关闭。 |
| `ALLOC_HOOK_DUMP_PREFIX` | 设置报告路径前缀。 |
| `BACKTRACE_DUMP_SIGNAL` | 覆盖平台默认的检查点信号。 |
| `ALLOC_HOOK_DEBUG` | 在 stderr 输出 hook 诊断信息。 |

采样只影响 host 分配归因；对通过平台导出策略和资源过滤的 mmap、DMA、ioctl
事件，资源记账仍然精确。

`ALLOC_HOOK_SAMPLING_INTERVAL_BYTES` 和 `ALLOC_HOOK_PEAK_SAMPLE_MS` 控制的是
两套不同机制：前者通过概率采样减少 host 堆栈抓取，后者启动独立的内存观察线程，
不会改变哪些分配被跟踪。

没有正数峰值采样间隔时，快照由跟踪到的存活分配字节数驱动。间隔为正数时，每轮
观察会读取 `/proc/self/status` 的当前 `VmRSS`、dmabuf 字节数，以及这两者都没有
覆盖的 GPU 设备映射，并以同一轮三者之和的最大值选择峰值窗口。若宿主框架提供了
名字以 `AUTO_SHOW_MEM_USE_DURATION_MS` 结尾且值为正数的环境变量，hook 会自动采用
该间隔；显式的 `ALLOC_HOOK_PEAK_SAMPLE_MS` 始终优先，包括值为 `0` 时。

观察线程不使用历史累计字段 `VmPeak` 或 `VmHWM`。当新的实测峰值越过快照门槛时，
它会立即再次读取 `VmRSS`、`RssAnon`、`RssFile`、`RssShmem`，从
`/proc/self/smaps` 收集驻留量最高的映射，然后复制存活堆栈表。这些操作顺序执行，
所以 `at_peak` 表示同一个回调窗口，而不是内核提供的原子快照。

要为每次采样到的新最大值保留堆栈快照，可使用：

```sh
export DUMP_PEAK_VALUE_MB=0
export ALLOC_HOOK_PEAK_SAMPLE_MS=5   # 使用外部采样器的间隔
export DUMP_PEAK_STEP_MB=0
export BACKTRACE_MIN_SIZE=1024
```

`DUMP_PEAK_STEP_MB=0` 最精确但开销更高；若可以接受 `at_snapshot` 和
`max_of_sum` 之间存在有界差距，就保留默认步进。只有确实需要 1 KB 以下分配的堆栈
时才设置 `BACKTRACE_MIN_SIZE=0`。要保持精确 host 归因且不抑制满足尺寸条件的堆栈，
两个字节采样变量都应保持未设置（实际值为 `1`）。

## 4. 预加载原生进程

```sh
mkdir -p ./trace
ALLOC_HOOK_CAPTURE_MODE=fast \
BACKTRACE_MIN_SIZE=4096 \
LD_LIBRARY_PATH="$PWD/out/lib" \
LD_PRELOAD="$PWD/out/lib/liballoc_hook.so" \
./your_program arg1 arg2
```

在 Android/OHOS 上，将库复制到目标设备的可写目录，并使用平台 shell 的预加载
方式。目标进程 ABI 必须与库一致。

拦截器覆盖导出的 C 分配函数族；OHOS 还会导出配置中的 C++ new/delete 族。
mmap 和资源 hook 受目标平台导出策略控制。直接系统调用和未导出的厂商入口会
绕过拦截器，这属于已知限制，不是抓栈失败。

## 5. 检查点和输出

导出的 C 函数 `checkpoint(const char*)` 会将存活分配报告写入指定路径。配置
信号会将同一工作排入专用 worker：

```sh
kill -<BACKTRACE_DUMP_SIGNAL> <pid>
```

报告包含 host/资源总量、分配大小和类型、时间戳、抓栈状态/错误、解析状态，以及
模块快照和符号器可用时的符号化栈。正常退出会在报告前刷新尚未处理的唯一栈。
`_exit`、致命信号和 `SIGKILL` 无法保证正常 worker 刷新。

请正确理解这些数值：

- Fast 采样记录的是估算后的 host 记账大小，不是驻留内存。
- `DMA+RSS Max (sampling)` 包含驻留映射和运行时开销，其中部分不在 hook
  的存活分配表中。
- host 和 DMA 的分量峰值可能发生在不同时间；应使用 hook 自身时间一致的总峰值，
  不要直接相加两个独立最大值。
- `observed_peak(at_snapshot)` 是触发当前保留堆栈快照的那轮采样；
  `observed_peak(max_of_sum)` 是整个运行中同轮总和的最大值。步进门槛抑制后续重建时，
  两者可能不同。
- `rss_breakdown(at_peak)` 和 `rss_by_mapping(at_peak)` 是为该峰值窗口立即收集的
  `/proc` 状态；如果没有抓到峰值上下文，标签会改为 `at_exit`。
- `partial` 抓栈、模块无法解析、队列丢弃和符号器失败都会以显式状态输出，不会
  伪造栈帧。

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
