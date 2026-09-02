# GPU 内存记账

[English](GPU_MEMORY_ACCOUNTING.md) · [中文 README](../README.zh-CN.md)

GPU 驱动交给进程的设备内存，会因为操作系统、厂商、驱动的分配路径以及内核版本不同而
被以完全不同的方式记账：有些进 `VmRSS`，有些进 dma-buf 统计，有些两者都不进。这篇文档
记录哪条路属于哪种，好让实测占用采样器的数字不需要靠猜来读，也免得后来的改动重复统计
一个已经被覆盖的量。

标注 **实测** 的行都是通过对应 API 真实分配、并对比分配前后 `/proc/self/status`、
`/proc/self/maps`、`/proc/self/smaps` 的差值得到的。标注 **推断** 的行没有实测，写在这里
是为了让缺口可见，而不是为了让它可依赖。

## 三个信号

采样器把三个量相加，而全部问题就在于它们并没有干净地切分设备内存。

| 信号 | 来源 | 能看见什么 |
| --- | --- | --- |
| `rss_bytes` | `/proc/self/status` 的 `VmRSS` | 内核在 `mm->rss_stat` 中计到本进程账上的页 |
| `dma_bytes` | `/proc/self/fd` + `fstat`，加上 `/proc/self/maps` 里的 dma-buf 映射 | 进程持有描述符或映射的 dma-buf 对象 |
| `gpu_bytes` | `/proc/self/maps` 里的设备节点映射 | 上面两者都覆盖不到的 GPU 字符设备映射 |

这张表里埋了两个坑。

**`VmRSS` 和 per-VMA 的 `Rss` 是两个不同的量。** `VmRSS` 来自 `mm->rss_stat`，内核只对
自己计到进程账上的页维护它；而 `smaps` 里 per-VMA 的 `Rss` 是遍历页表数 present 项算出来
的。对普通匿名映射两者一致；对 GPU 字符设备映射，两者可以完全不一致 —— 实测中有一台设备
把某个区间的 `Rss` 报成等于它的 `Size`，而 `VmRSS` 一点没动。任何建立在 `Size - Rss` 之上
的公式，因此会对"完全不被 `rss_bytes` 看见"的那部分内存报出 0。

**dma-buf 一不小心就会被算两遍。** 它可以通过描述符、通过映射，还可能通过再导入某个 GPU
API 被触达。`dma_bytes` 之所以按 inode 去重就是为了这个，而 `gpu_bytes` **绝不能**再把
dma-buf 支撑的内存加进去。

## 实测设备

`gpu_model` 取自 `/sys/class/kgsl/kgsl-3d0/gpu_model`；`soc.model` 取自
`getprop ro.soc.model`；`machine` 取自 `/sys/bus/soc/devices/soc0/machine`；内核和
架构取自 `uname`。

| # | gpu_model | soc.model | machine | 内核 | Android | arch | 设备节点 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | Adreno662v2 | SM7450 | DIWALI | 5.10 | 12 | aarch64 | `/dev/kgsl-3d0` |
| A2 | Adreno720 | SM7550 | CROW | 5.15 | 14 | aarch64 | `/dev/kgsl-3d0` |
| A3 | Adreno830v2 | SM8750 | SUN | 6.6 | 15 | aarch64 | `/dev/kgsl-3d0` |
| A4 | Adreno840v2 | SM8850 | CANOE | 6.12 | 16 | aarch64 | `/dev/kgsl-3d0` |
| A5 | Adreno850v2 | SM8975 | ART | 6.18 | 17 | aarch64 | `/dev/kgsl-3d0` |
| M1 | Mali-G925-Immortalis MC12 | MT6991 | MT6991 | 6.6 | 15 | aarch64 | `/dev/mali0` |
| M2 | Mali-G1-Ultra MC12 | MT6993 | MT6993 | 6.12 | 16 | aarch64 | `/dev/mali0` |
| H1 | — | — | — | HongMeng 1.13.0 | HarmonyOS | aarch64 | 两者都无 |

另有一台 6.6 内核、无用户态 GPU 栈的 Adreno 设备记为 **A0**，只用于"驱动 ioctl"那一行，
因为它上面没有 OpenCL。

## 实测行为

64 MB 分配，数值为调用前后的差值。"设备映射"指 `/proc/self/maps` 里对 GPU 字符设备的映射。

### `clCreateBuffer`，以及随后 map + touch

| 设备 | create 时 | map + touch 后 | 结论 |
| --- | --- | --- | --- |
| A1 | 设备映射 +64 MB，其 `Rss` **+64 MB**，`VmRSS` **+0** | 完全无变化 | `VmRSS` 永远不算它；应贡献全额 mapped size |
| A2 | 设备映射 +64 MB，其 `Rss` +0，`VmRSS` +0 | `VmRSS` **+64 MB**，其 `Rss` **+64 MB** | `VmRSS` 会算它，但只在 fault 之后 |
| A3、A4、A5 | **完全没有设备映射** | `VmRSS` +64 MB | 就是普通驻留内存，这一维无需增加任何东西 |
| M1、M2 | 设备映射 +64 MB，其 `Rss` +0，`VmRSS` **+64 MB** | 无变化 | `VmRSS` 在 create 时就算了它；再加映射就是重复计数 |
| A0 | 设备映射，`Rss` 和 `VmRSS` 都随 fault 增长 | — | `VmRSS` 会算它 |

`CL_MEM_ALLOC_HOST_PTR` 在每台设备上的表现都与普通 `CL_MEM_READ_WRITE` 完全一致。
`clEnqueueMapBuffer` 从不分配内存：凡是有变化的，变的都是 touch 引起的 fault，而不是 map
调用本身。

**五颗 Adreno 出现三种不同行为**是最值得注意的一点，而分界线是内核/驱动代次而非厂商：最老
的两颗会建立设备映射且彼此对 `VmRSS` 的结论相反，最新的三颗**根本不建立设备映射**。也就是
说任何以厂商字符串、或以"设备节点是否存在"为判据的规则，在这张表里都会在某颗芯片上出错。

这也说明"映射总量增长时 `VmRSS` 是否跟着动"这种推断**不够**。在 A2 上映射出现在某次采样，
而 fault 发生在更晚，所以在增长那一步 `VmRSS` 还没动，看起来和 A1 一样 —— 但同一批页片刻
之后就被 `VmRSS` 算进去了。要区分两者需要 per-region 的驻留量，而这恰恰是 A1 打破的：在
A1 上 per-region `Rss` 报全额，而 `VmRSS` 一点不报。

### dma-buf，以及厂商对它的导入

| 路径 | 设备 | `VmRSS` | 设备映射 | dma-buf 映射 |
| --- | --- | --- | --- | --- |
| dma-heap 分配，只持描述符 | 全部 | +0 | +0 | +0 |
| 对该 dma-buf `mmap` + touch | 全部 | **+0** | +0 | **+64 MB，`Rss` 为 0** |
| `CL_MEM_DMABUF_HOST_PTR_QCOM` 导入 | A1–A5 | +0 | +0 | +0 |
| `CL_MEM_ION_HOST_PTR_QCOM` 导入 | A1–A5 | +0 | +0 | +0 |
| 导入 + map + touch | A1–A5 | +0 | +0 | +0 |
| **`clImportMemoryARM`**（`CL_IMPORT_TYPE_DMA_BUF_ARM`）| M1、M2 | +128 kB | **+64 MB** | **+64 MB** |

所有 dma-buf 路径在全部七台设备上都对 `VmRSS` 不可见，并表现为一个 `Rss` 为 0 的 dma-buf
映射，因此 `dma_bytes` 已经覆盖。两种 QCOM 导入都只是绑定已有对象，不分配内存。

`clImportMemoryARM` 是例外，也正是 Mali 设备节点必须继续排除在这一维之外的原因：这次导入
对同一块 64 MB **同时**建立了一个 `/dev/mali0` 映射和一个 dma-buf 映射。如果这一维去统计
Mali 设备节点，那块内存就会被报两遍 —— 一遍在这里，一遍在已经按描述符统计它的 `dma_bytes`。

### OHOS

H1 跑的是 HongMeng 内核，不是 Linux。它两个 GPU 字符设备都没有，同时具备 `/dev/ion` 和
`/dev/dma_heap`，并且提供带 `VmRSS` 的 `/proc/self/status`、以及 `maps`、`smaps`、
`pagemap`。`/vendor/lib64` 下有 `libOpenCL.so`。上面那些分配路径**没有**在它上面跑过 ——
需要一个面向 OHOS 的构建 —— 所以不给出结论行。已确立的是：设备节点扫描在那里什么都找不到，
这对一个没有该节点的平台正是正确结果；而采样器依赖的那些 `/proc` 接口都存在。

## 厂商 API 注意事项

这些重新踩一遍要花真金白银的时间，所以记在这里。

`CL_MEM_EXT_HOST_PTR_QCOM` **必须**和 `CL_MEM_USE_HOST_PTR` 一起用。缺这个 flag 时，无论
用哪种 cache policy、哪种 host-pointer 结构体，一律返回 `CL_INVALID_VALUE`，而且错误码
不会告诉你缺的是哪个 flag。能工作的调用是
`CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM`，host-pointer
结构体作为 `host_ptr` 传入。

`CL_MEM_DMABUF_HOST_PTR_QCOM` 在一台 `CL_DEVICE_EXTENSIONS` 里**没有**声明
`cl_qcom_dmabuf_host_ptr`（但声明了 `cl_qcom_ion_host_ptr`）的设备上照样能用。靠扩展
字符串选择导入路径的代码在那台设备上会走 ION 分支，尽管 dma-buf 分支其实是通的。两者的
记账结果相同，所以这是可移植性提示，不是正确性问题。

`CL_DEVICE_EXT_MEM_PADDING_IN_BYTES_QCOM` 在实测设备上返回 0、
`CL_DEVICE_PAGE_SIZE_QCOM` 返回 4096，两个查询的返回码都是成功。**要检查返回码**：查询
失败时不会改写调用方的变量，如果那个变量初始化成 0，失败和真实的 0 就无法区分。

`EGL_EXT_image_dma_buf_import` 在两台实测设备上都**没有**声明。Mali 那台照样接受了
`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)`；Adreno 那台对试过的每种格式都返回
`EGL_BAD_PARAMETER`。由于导入本身不分配内存，两种结果都不改变记账。

## 未实测

明确写出来，以便上面那张表的边界是清楚的。

`clImportMemoryARM`（`cl_arm_import_memory` / `cl_arm_import_memory_dma_buf`）是 Mali 和
PowerVR 上对应 QCOM host-pointer 导入的接口，实测的 Mali 设备声明了它。**推断**：和 QCOM
的导入一样，它绑定一个已存在的 dma-buf 而非分配，所以那块内存应该已经在 `dma_bytes` 里，
且不会新增设备节点映射。**这一条没有验证**，而且它是 Mali 上最可能出意外的地方 —— 因为
Mali 自己的工作缓冲就是 dma-buf，而 Adreno 的盲区是字符设备，两者机制不同。

PowerVR 完全没测。做这类导入的宿主框架一般把它和 ARM 归到同一条 import 路径，暗示预期
相同，但同样处于未验证状态。

OHOS 没测。这个 hook 支持 OHOS，而且在那里默认**关闭** `mmap` interposition，所以厂商
GPU 栈建立的设备节点映射会被 `/proc/self/maps` 扫描看到，但不会被分配 hook 看到。OHOS 的
GPU 驱动用字符设备、用 dma-buf 等价物、还是别的什么，这里未知。

受保护/安全堆（`CL_MEM_DMABUF_HOST_PTR_PROTECTED_QCOM`、`qcom,secure-*` 和 `mtk_prot_*`
这些 dma-heap）没测。它们可能根本不允许普通进程映射，那种情况下采样器里没有任何信号能
看见它们，也就不应该有任何信号声称看见了。

Vulkan、以及不是 dma-buf 导入的 GL 纹理，都没测。

## 怎么往这张表里加新行

用的探针都是很小的独立程序：通过目标 API 分配，分配前后各快照一次 `VmRSS`、以及设备节点
映射和 dma-buf 映射的 mapped size 与 per-region `Rss`，然后打印差值。产出上面这些行的过程
中有两条是踩出来的：

**先搞清楚"present"是什么意思再去信它。** `mincore` 报的是页缓存里在不在，`pagemap` 报的
是页表里在不在；在 Adreno/OpenCL 上，这两者都把设备映射报成"完全驻留"，而 `VmRSS` 把它
完全排除。它们都回答不了这里唯一重要的问题 —— `VmRSS` 到底算不算。两个都被当成 `smaps`
的便宜替代品试过，两个都是错的。

**永远量差值，不要量绝对值。** GPU 驱动会在创建 context 时映射自己的工作集，所以任何在
context 存在之前取的基线，都会把驱动的内存记到接下来测的那个东西头上。
