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
cmake -S . -B build-host \
  -DCMAKE_BUILD_TYPE=Release \
  -DMALLOC_HOOK_ENABLE_RESOURCE_TRACKING=OFF
cmake --build build-host --target alloc_hook
ctest --test-dir build-host --output-on-failure
cmake --install build-host --prefix "$PWD"
```

生成的库为 `out/lib/liballoc_hook.so`。

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
| `ALLOC_HOOK_DEBUG_SIGNAL` | 在 stderr 输出信号 worker 诊断。 |

采样只影响 host 分配归因；对通过平台导出策略和资源过滤的 mmap、DMA、ioctl
事件，资源记账仍然精确。

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
