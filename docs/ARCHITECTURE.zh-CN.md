# 架构

[English architecture](ARCHITECTURE.md) · [中文 README](../README.zh-CN.md)

## 数据路径

```text
分配 hook
    -> 有界原始栈采集
    -> PointerData 存活分配表
    -> AsyncStackPipeline 队列
       -> 模块快照解析器
       -> 符号化器
       -> 完成结果
    -> 检查点 / 峰值报告

独立实测内存采样器（可选）
    -> 当前 VmRSS + DMA + 未覆盖的 GPU 映射
    -> 同一轮采样总和的峰值门槛
    -> /proc 峰值上下文 + PointerData 存活堆栈快照
```

分配 hook 只执行有界工作：记录请求大小和内存类型，采集原始程序计数器，并将原始记录副本放入队列。模块查询和符号化明确放在 hook 路径之外。

## 抓栈契约

`CaptureStack()` 返回项目自有的 `RawStackRecord`，其中包含采集状态、模式、后端、终止错误、跳过帧数、模块代次和有界 PC 数组。

- **Fast**：配置阶段的编译器能力探测成功时使用 `_Unwind_Backtrace`。它只针对当前线程，采集有界原始 PC，不做同步符号查询。
- **Accurate**：选择明确的平台后端，并可保留有用的部分栈和终止错误；fallback 必须显式，不能静默改变模式或平台。

核心契约覆盖原生 C/C++ 栈。托管运行时栈、其他线程上下文和离线 unwind 输入属于可选能力，不能作为默认前提。

## 异步解析

`AsyncStackPipeline` 按原始 PC 和模块代次对记录去重。当前提供的
`NativeModuleResolver` 通过 `dl_iterate_phdr` 对已加载 ELF 装载段建立快照，
不是离线文件读取器。当前提供的 `NativeSymbolizer` 在 worker 中使用
`dladdr` 获取动态符号名，并始终保留原始 PC 和模块相对 PC；当前实现不承诺
完整的 DWARF/debug 文件或离线符号化。`Symbolizer` 将模块和 PC 转换为
`SymbolizedFrame`。队列容量、重复、丢弃、递归提交、worker 启动失败和已处理
结果通过 `AsyncStackStats` 暴露。

完成回调收到包含原始记录、解析状态和符号化帧的 `StackResult`。hook 边界通过 `AsyncStackWorkerThread()` 防止解析器内部被递归统计。

## 统计和报告

`PointerData` 管理存活分配表和峰值计数。Host 分配可以使用 Fast 专用 Poisson 字节采样；资源路径保持精确。采样 host 记录保存估算后的统计大小，释放时仍通过同一指针身份路径移除。

检查点报告由导出的 `checkpoint(const char*)` 入口或配置的信号触发。使用 `DUMP_PEAK_VALUE_MB` 启用峰值快照，并由 `DUMP_PEAK_STEP_MB` 限制重建频率。

默认峰值判据是跟踪到的分配字节数。将 `ALLOC_HOOK_PEAK_SAMPLE_MS` 设为正数后，
判据改为同一轮采样中当前 `VmRSS`、dmabuf 字节数和前两者未覆盖的 GPU 映射之和的
最大值。采样器使用独立线程，因为页面驻留状态和设备内存所有权可能在没有任何分配
hook 调用时变化。它读取的是当前 `VmRSS`，而不是历史累计的 `VmPeak` 或 `VmHWM`。

实测样本越过快照门槛后，回调会再次读取 `/proc/self/status`（`VmRSS`、
`RssAnon`、`RssFile`、`RssShmem`），从 `/proc/self/smaps` 收集驻留量最高的映射，
然后复制存活堆栈表。这样可把昂贵的 `/proc` 工作留在采样线程，但整个序列并非原子
操作：报告中标为 `at_peak` 的值属于同一个回调窗口，不代表内核的单一快照时刻。

## 平台边界

CMake 将目标 OS、libc、架构、编译器 unwind 能力和导出策略分离。Android 与 glibc Linux 按策略导出 mmap hook；OHOS 默认关闭 mmap hook，并提供仅用于受控复现的构建开关。资源 hook 只有在启用资源跟踪构建特性时才会编译。

## 扩展指导

保持原始采集记录和异步解析接口的平台无关性。平台差异应放在
`CaptureStack` 后端、`ModuleResolver` 实现或 `Symbolizer` 实现中。未来的
离线 ELF/DWARF 符号器应在 worker 侧消费保留的模块身份和原始/模块相对 PC。
不要将模块查询、动态分配或符号化加入分配 hook 路径，除非同步更新采集和
递归契约。
