# GPU memory accounting

[中文 / Chinese](GPU_MEMORY_ACCOUNTING.zh-CN.md) · [English README](../README.md)

Device memory that a GPU driver hands to a process is accounted differently
depending on the OS, the vendor, the driver's allocation path, and the kernel
version. Some of it lands in `VmRSS`, some in dma-buf accounting, some in
neither. This page records which is which, so the observed-footprint sampler can
be read without guessing and so a future change does not re-add a term that is
already counted.

Every row marked **measured** was observed by allocating through the API in
question and reading the deltas in `/proc/self/status`, `/proc/self/maps` and
`/proc/self/smaps` across the allocation. Rows marked **inferred** are not
measured; they are stated so the gap is visible, not so it can be relied on.

## The three signals

The sampler sums three quantities, and the whole problem is that they do not
partition device memory cleanly.

| Signal | Read from | Sees |
| --- | --- | --- |
| `rss_bytes` | `VmRSS` in `/proc/self/status` | pages the kernel accounts to this process in `mm->rss_stat` |
| `dma_bytes` | `/proc/self/fd` + `fstat`, plus dma-buf mappings in `/proc/self/maps` | dma-buf objects the process holds a descriptor for or a mapping of |
| `gpu_bytes` | device-node mappings in `/proc/self/maps` | GPU character-device mappings that neither of the above covers |

Two traps live in this table.

**`VmRSS` and per-VMA `Rss` are different quantities.** `VmRSS` comes from
`mm->rss_stat`, which the kernel maintains only for pages it accounts to the
process. Per-VMA `Rss` in `smaps` is produced by walking the page tables and
counting present entries. For an ordinary anonymous mapping the two agree. For a
GPU character-device mapping they can disagree completely: one measured target
reports a region's `Rss` as its full `Size` while `VmRSS` does not move at all.
Any formula built on `Size - Rss` therefore reports zero for memory that is
entirely invisible to `rss_bytes`.

**A dma-buf can be counted twice if you are not careful.** It is reachable
through a descriptor, through a mapping, and potentially through a second import
into a GPU API. `dma_bytes` deduplicates by inode for exactly this reason, and
`gpu_bytes` must not add dma-buf backed memory at all.

## Measured behaviour

Targets: **Adreno/OpenCL** is an Adreno 662 Android target; **Adreno/ioctl** is a
different Adreno target on a 6.6 kernel with no userspace GPU stack, allocating
straight through the driver's allocation ioctl; **Mali** is a Mali-G925 Android
target. Sizes are the allocation, deltas are across it.

| Path | Target | In `VmRSS` | Appears as | Covered by |
| --- | --- | --- | --- | --- |
| `clCreateBuffer(CL_MEM_READ_WRITE)` | Adreno/OpenCL | **no** | device-node mapping, full size | `gpu_bytes` |
| `clCreateBuffer(… \| CL_MEM_ALLOC_HOST_PTR)` | Adreno/OpenCL | **no** | device-node mapping, full size | `gpu_bytes` |
| `clEnqueueMapBuffer` on either of the above | Adreno/OpenCL | no change | nothing new | — |
| driver allocation ioctl, all four cache modes | Adreno/ioctl | **yes**, as it faults | device-node mapping | `rss_bytes` |
| `clCreateBuffer(CL_MEM_READ_WRITE)` | Mali | **yes**, in full | nothing | `rss_bytes` |
| `clCreateBuffer(… \| CL_MEM_ALLOC_HOST_PTR)` | Mali | **yes**, in full | nothing | `rss_bytes` |
| dma-heap allocation, descriptor only | both | no | nothing | `dma_bytes` (by descriptor) |
| dma-heap allocation, `mmap` + touch | both | **no** | dma-buf mapping, `Rss` 0 | `dma_bytes` |
| `clCreateBuffer(CL_MEM_EXT_HOST_PTR_QCOM)` importing that dma-buf | Adreno/OpenCL | no change | **nothing new** | already `dma_bytes` |
| `clEnqueueMapBuffer` on the imported buffer | Adreno/OpenCL | no change | nothing new | — |
| `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` importing a dma-buf | Mali | no change beyond driver overhead | nothing new | already `dma_bytes` |

Three conclusions follow.

**Only the device-node mapping is a genuine blind spot.** Every dma-buf backed
path is already counted, and importing a dma-buf into a GPU API adds no mapping
of its own — the import binds the existing object rather than allocating. So
`gpu_bytes` needs to cover device-node mappings and nothing else, which is also
why it costs one `/proc/self/maps` read and no more.

**Vendor is the wrong axis.** The two Adreno targets disagree with each other and
one of them agrees with Mali. Whether these mappings are counted in `VmRSS` is a
function of the driver's allocation path and the kernel, so the sampler
determines it at runtime rather than keying off the vendor string. It watches
whether `VmRSS` moves when the mapped total grows, using the two signals it
already reads every tick, and reports the conclusion as `gpu_rss` in
`observed_sampler`.

**`clEnqueueMapBuffer` is not the allocation point.** On the Adreno/OpenCL target
the mapping appears at `clCreateBuffer`; mapping and touching it afterwards adds
nothing. Anything keyed on the map call would see the memory late or not at all.

## Vendor API notes

These cost real time to rediscover, so they are recorded here.

`CL_MEM_EXT_HOST_PTR_QCOM` requires `CL_MEM_USE_HOST_PTR` alongside it. Without
that flag every variant returns `CL_INVALID_VALUE`, regardless of cache policy or
which host-pointer struct is used, and the error does not indicate which flag is
missing. The working call is
`CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR | CL_MEM_EXT_HOST_PTR_QCOM` with the
host-pointer struct passed as `host_ptr`.

`CL_MEM_DMABUF_HOST_PTR_QCOM` worked on a target whose `CL_DEVICE_EXTENSIONS`
does **not** advertise `cl_qcom_dmabuf_host_ptr`, while `cl_qcom_ion_host_ptr`
was advertised. Code that selects the import path from the extension string will
take the ION path there even though the dma-buf path is available. Both produce
the same accounting outcome, so this is a portability note rather than a
correctness one.

`CL_DEVICE_EXT_MEM_PADDING_IN_BYTES_QCOM` returned 0 and
`CL_DEVICE_PAGE_SIZE_QCOM` returned 4096 on the measured target, both with a
success return code. Check the return code: a query that fails leaves the
caller's variable untouched, which is indistinguishable from a real 0 if it was
initialised to 0.

`EGL_EXT_image_dma_buf_import` was not advertised on either measured target. The
Mali target accepted `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` anyway; the
Adreno target rejected it with `EGL_BAD_PARAMETER` for every format tried. Since
the import allocates nothing, neither outcome changes what is accounted.

## Not measured

Stated so the boundary of the table above is explicit.

`clImportMemoryARM` (`cl_arm_import_memory` / `cl_arm_import_memory_dma_buf`) is
the Mali and PowerVR equivalent of the QCOM host-pointer imports, and is
advertised on the measured Mali target. **Inferred**: like the QCOM imports it
binds an existing dma-buf rather than allocating, so the memory should already be
in `dma_bytes` and it should add no device-node mapping. This has not been
verified, and it is the most likely place for a surprise on Mali, because Mali's
own working buffers are dma-bufs whereas the Adreno blind spot is a character
device.

PowerVR has not been measured at all. The host frameworks that do this import
route it through the same ARM path, which suggests the same expectation, on the
same unverified footing.

OHOS has not been measured. The hook supports it, and it leaves `mmap`
interposition off by default there, so a device-node mapping created by a vendor
GPU stack would be visible to the `/proc/self/maps` scan but not to the
allocation hook. Whether OHOS GPU drivers use a character device, a dma-buf
equivalent, or something else is unknown here.

Protected and secure heaps (`CL_MEM_DMABUF_HOST_PTR_PROTECTED_QCOM`, the
`qcom,secure-*` and `mtk_prot_*` dma-heaps) have not been measured. They may not
be mappable by a normal process at all, in which case nothing in the sampler can
see them and no signal should claim to.

Vulkan, and GL textures that are not dma-buf imports, have not been measured.

## How to extend this table

The probes used are small standalone programs: allocate through the API, snapshot
`VmRSS` plus the mapped size and per-region `Rss` of both device-node and dma-buf
mappings, allocate, snapshot again, print the deltas. Two rules learned the hard
way while producing the rows above:

Check what "present" means before trusting it. `mincore` reports page-cache
presence and `pagemap` reports page-table presence; on the Adreno/OpenCL target
both report a device mapping as fully present while `VmRSS` excludes it entirely.
Neither answers the only question that matters here, which is what `VmRSS`
counts. Both were tried as cheap substitutes for `smaps` and both were wrong.

Measure the delta, never the absolute. A GPU driver maps its own working set at
context creation, so any baseline taken before the context exists attributes the
driver's memory to whatever is measured next.
