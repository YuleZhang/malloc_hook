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

## 实测行为

测试对象：**Adreno/OpenCL** 是一台 Adreno 662 的 Android 设备；**Adreno/ioctl** 是另一台
Adreno 设备，6.6 内核、没有用户态 GPU 栈，直接走驱动的分配 ioctl；**Mali** 是一台
Mali-G925 的 Android 设备。尺寸是分配量，数值是分配前后的差值。

| 路径 | 设备 | 进 `VmRSS` | 表现为 | 由谁覆盖 |
| --- | --- | --- | --- | --- |
| `clCreateBuffer(CL_MEM_READ_WRITE)` | Adreno/OpenCL | **否** | 设备节点映射，全额 | `gpu_bytes` |
| `clCreateBuffer(… \| CL_MEM_ALLOC_HOST_PTR)` | Adreno/OpenCL | **否** | 设备节点映射，全额 | `gpu_bytes` |
| 对上面两者 `clEnqueueMapBuffer` | Adreno/OpenCL | 无变化 | 无新增 | — |
| 驱动分配 ioctl，四种 cache mode | Adreno/ioctl | **是**，随 fault 增长 | 设备节点映射 | `rss_bytes` |
| `clCreateBuffer(CL_MEM_READ_WRITE)` | Mali | **是**，全额 | 无 | `rss_bytes` |
| `clCreateBuffer(… \| CL_MEM_ALLOC_HOST_PTR)` | Mali | **是**，全额 | 无 | `rss_bytes` |
| dma-heap 分配，只持描述符 | 两者 | 否 | 无 | `dma_bytes`（按描述符）|
| dma-heap 分配，`mmap` + touch | 两者 | **否** | dma-buf 映射，`Rss` 为 0 | `dma_bytes` |
| `clCreateBuffer(CL_MEM_EXT_HOST_PTR_QCOM)` 导入该 dma-buf | Adreno/OpenCL | 无变化 | **无新增** | 已在 `dma_bytes` |
| 对导入的 buffer `clEnqueueMapBuffer` | Adreno/OpenCL | 无变化 | 无新增 | — |
| `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` 导入 dma-buf | Mali | 除驱动自身开销外无变化 | 无新增 | 已在 `dma_bytes` |

由此得到三条结论。

**只有设备节点映射是真正的盲区。** 所有 dma-buf 支撑的路径都已被覆盖，而把 dma-buf 导入
GPU API **不会**新增任何映射 —— 导入是绑定一个已存在的对象，不是分配。所以 `gpu_bytes`
只需要覆盖设备节点映射，别的都不用管，这也正是它只要读一次 `/proc/self/maps` 的原因。

**按厂商分类是错的轴。** 两台 Adreno 彼此结论相反，而其中一台和 Mali 一致。这类映射是否
计入 `VmRSS`，取决于驱动的分配路径和内核，所以采样器在运行时判定，而不是看厂商字符串。
判定方式是：观察映射总量增长时 `VmRSS` 是否跟着动 —— 用的就是它每次采样本来就读的那两个
信号 —— 结论以 `gpu_rss` 报在 `observed_sampler` 里。

**`clEnqueueMapBuffer` 不是分配点。** 在 Adreno/OpenCL 上映射是在 `clCreateBuffer` 时就
建立的，之后 map 再 touch 一个字节都不增加。任何以 map 调用为触发点的逻辑都会晚看到、
甚至看不到这块内存。

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
