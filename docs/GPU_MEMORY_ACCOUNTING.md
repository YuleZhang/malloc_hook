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

## Targets measured

`gpu_model` is `/sys/class/kgsl/kgsl-3d0/gpu_model`; `soc.model` is
`getprop ro.soc.model`; `machine` is `/sys/bus/soc/devices/soc0/machine`; kernel
and arch are from `uname`.

| # | gpu_model | soc.model | machine | kernel | Android | arch | device node |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | Adreno662v2 | SM7450 | DIWALI | 5.10 | 12 | aarch64 | `/dev/kgsl-3d0` |
| A2 | Adreno720 | SM7550 | CROW | 5.15 | 14 | aarch64 | `/dev/kgsl-3d0` |
| A3 | Adreno830v2 | SM8750 | SUN | 6.6 | 15 | aarch64 | `/dev/kgsl-3d0` |
| A4 | Adreno840v2 | SM8850 | CANOE | 6.12 | 16 | aarch64 | `/dev/kgsl-3d0` |
| A5 | Adreno850v2 | SM8975 | ART | 6.18 | 17 | aarch64 | `/dev/kgsl-3d0` |
| M1 | Mali-G925-Immortalis MC12 | MT6991 | MT6991 | 6.6 | 15 | aarch64 | `/dev/mali0` |
| M2 | Mali-G1-Ultra MC12 | MT6993 | MT6993 | 6.12 | 16 | aarch64 | `/dev/mali0` |
| H1 | — | — | — | HongMeng 1.13.0 | HarmonyOS | aarch64 | neither |

A separate Adreno target on a 6.6 kernel with no userspace GPU stack is referred
to as **A0**; it is used only for the driver-ioctl row, since it has no OpenCL.

## Measured behaviour

64 MB allocations, deltas across the call. "device mapping" means a mapping of the
GPU character device in `/proc/self/maps`.

### `clCreateBuffer`, and mapping plus touching it

| Target | at create | after map + touch | verdict |
| --- | --- | --- | --- |
| A1 | device mapping +64 MB, its `Rss` **+64 MB**, `VmRSS` **+0** | no change at all | `VmRSS` never counts it; contribution is the full mapped size |
| A2 | device mapping +64 MB, its `Rss` +0, `VmRSS` +0 | `VmRSS` **+64 MB**, its `Rss` **+64 MB** | `VmRSS` counts it, but only once faulted |
| A3, A4, A5 | **no device mapping at all** | `VmRSS` +64 MB | ordinary resident memory; nothing for this dimension to add |
| M1, M2 | device mapping +64 MB, its `Rss` +0, `VmRSS` **+64 MB** | no change | `VmRSS` counts it at create; adding the mapping would double count |
| A0 | device mapping, `Rss` and `VmRSS` both track faulting | — | `VmRSS` counts it |

`CL_MEM_ALLOC_HOST_PTR` behaved identically to plain `CL_MEM_READ_WRITE` on every
target. `clEnqueueMapBuffer` never allocated: where anything changed it was the
faulting caused by touching, not the map call.

Four distinct behaviours across five Adreno parts is the headline. **The device
mapping only exists on the older two.** On Adreno 830 and newer an OpenCL buffer
is ordinary `VmRSS` memory with no device mapping, so this dimension is
structurally zero there. Of the two that do create one, A1 and A2 disagree with
each other about `VmRSS`.

That is also why an inference of the form "did `VmRSS` move when the mapped total
grew" is not sufficient, and it was tried and discarded. On A2 the mapping appears
in one sample and the faulting happens later, so at the growth step `VmRSS` has
not moved yet and the sample is indistinguishable from A1 — while the same pages
are counted in `VmRSS` a moment later, which would double count them. The
arithmetic that does separate them is below.

### dma-buf, and the vendor imports of it

| Path | Targets | `VmRSS` | device mapping | dma-buf mapping |
| --- | --- | --- | --- | --- |
| dma-heap alloc, descriptor only | all | +0 | +0 | +0 |
| that dma-buf `mmap` + touch | all | **+0** | +0 | **+64 MB, `Rss` 0** |
| `CL_MEM_DMABUF_HOST_PTR_QCOM` import | A1–A5 | +0 | +0 | +0 |
| `CL_MEM_ION_HOST_PTR_QCOM` import | A1–A5 | +0 | +0 | +0 |
| import + map + touch | A1–A5 | +0 | +0 | +0 |
| **`clImportMemoryARM`** (`CL_IMPORT_TYPE_DMA_BUF_ARM`) | M1, M2 | +128 kB | **+64 MB** | **+64 MB** |

Every dma-buf path is invisible to `VmRSS` and visible as a dma-buf mapping with
`Rss` 0, on all seven targets, so `dma_bytes` covers it. Both QCOM imports bind
the existing object and allocate nothing.

`clImportMemoryARM` is the exception and the reason the Mali device node must stay
excluded from this dimension. The import creates **both** a `/dev/mali0` mapping
and a dma-buf mapping for the same 64 MB. A dimension that counted the Mali
device node would therefore report that memory twice: once here and once in
`dma_bytes`, which already counts it by descriptor.

### OHOS

H1 runs a HongMeng kernel, not Linux, on a Hisilicon GPU. It exposes neither GPU
character device, carries both `/dev/ion` and `/dev/dma_heap`, and provides
`/proc/self/status` with `VmRSS`, `maps`, `smaps` and `pagemap`.

It is a **fourth mechanism**, and the current implementation does not see it.
A 256 MB `clCreateBuffer` plus map plus touch produces:

```text
   4096 MB  6f00000000-7000000000 ---p  [io]   <- sparse VA reservation, PROT_NONE
    256 MB  70cff9a000-70dff9b000 rw-p  [io]   <- the allocation
```

There is no device node in the path, and searching for one found nothing to scan
for. `/dev` on this target carries no GPU render node — no `mali`, no `kgsl`, no
`dri/render*`; the closest entries are `graphics/{fb0,dpu_res}` and some
`hisi_*` control nodes. With a live OpenCL context, a 256 MB buffer allocated and
written, the process holds **no GPU device descriptor at all**: the only
device-like fds open are `/dev/kmsg` and `/dev/null`. And the mapping itself
records `dev 00:00 inode 0`, so it is not file-backed either:

```text
6f00000000-7000000000 ---p 00000000 00:00 0    [io]
70cff9a000-70dff9b000 rw-p 00000000 00:00 0    [io]
```

So the allocation does not travel through a character device this process opens.
The kernel names the result `[io]`, which is what it calls a PFN/IO mapping, and
the accounting follows from that:

| state | `[io]` rw mapped | its `Rss` | Σ all `Rss` | `VmRSS` | divergence |
| --- | --- | --- | --- | --- | --- |
| baseline | 400 | 0 | 38088 | 38080 | 8 |
| after create | 262548 | 0 | 38156 | 38148 | 8 |
| after map + touch | 262548 | **0** | 38344 | 38336 | **8** |

So 256 MB is allocated and written, and **every residency interface reports
nothing**: `VmRSS` does not count it, and neither does per-VMA `Rss`. Because both
agree, there is no divergence to measure, and the arithmetic above evaluates to
zero. The only signal that sees this memory at all is the region's mapped size.

That is a different failure from A1. On A1 the two interfaces disagree and the
disagreement *is* the answer. Here they agree, and both are blind.

Worse, this state is indistinguishable from Mali by residency alone: on both, the
device region reports `Rss` 0 and a full mapped size. The answers required are
opposite — Mali's pages are in `VmRSS` and must contribute nothing, H1's are in
nothing and must contribute their mapped size. What separates them is whether
`VmRSS` grew when the mapping appeared, which on Mali happens in the same step as
the mapping and on A2 happens a step later. No single one of the three signals
resolves all four targets.

Two consequences, both open:

* The scan is scoped to device-node paths, so it does not match `[io]` at all and
  reports zero on this platform. Widening it must exclude the `PROT_NONE`
  reservation, which is 4 GB here and would dwarf any real figure; the `rw` versus
  `---` permission split separates them cleanly.
* The arithmetic needs a term the divergence cannot supply. Nothing is shipped for
  this yet, because the rule that covers A1, A2, Mali and H1 together has not been
  established, and a rule verified on three of four is how the previous two
  attempts went wrong.

The dma-heap allocation used elsewhere in this page also failed here: the heap
names differ from both Android vendors, so that row is unmeasured on H1.

## What the sampler computes

`VmRSS` is supposed to equal the sum of per-VMA `Rss`. Where a device mapping's
pages are counted by the page-table walk that produces per-VMA `Rss` but not by
the `mm->rss_stat` counters behind `VmRSS`, the two diverge by exactly the amount
`rss_bytes` cannot see. So:

```text
divergence = max(sum of all per-VMA Rss - VmRSS, 0)
gpu_bytes  = min(divergence, sum of per-VMA Rss over device regions)
```

Measured readings, in kB as `/proc` reports them:

| Target | state | Σ all `Rss` | `VmRSS` | divergence | device `Rss` | `gpu_bytes` | correct? |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | after create | 83332 | 17740 | 65592 | 65576 | **65576** | yes — `VmRSS` sees none of it |
| A2 | after create | 21520 | 21408 | 112 | 40 | 40 | yes — nothing backed yet |
| A2 | after touch | 87060 | 86868 | 192 | 65576 | 192 | yes — `VmRSS` now holds it |
| M1 | after create | 30340 | 100680 | 0 | 0 | **0** | yes — already in `VmRSS` |
| A3–A5 | after create | — | — | — | 0 | **0** | yes — no device mapping |

The floor at zero matters: on M1 the divergence runs the other way, `VmRSS`
counting pages the per-VMA walk does not. The bound by device `Rss` matters
because a process carries unrelated divergence (112 kB on A2 at rest) that must
not be attributed here.

This needs smaps, which no cheap substitute replaces. It is read only when the
device mapped total changes — the event that can change the answer — and the
figure is carried between those events, so a run whose GPU buffers are allocated
once pays for one read. A process with the device node but no device mapping pays
none. `gpu_reads` in `observed_sampler` reports how many were paid.

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

Sum the per-VMA `Rss` and compare it against `VmRSS`. Those two are supposed to be
the same number, and where they are not, the difference is memory one kernel
interface counts and the other does not. That comparison is what finally separated
A1 from A2 after two cheaper discriminators had failed, and it is the first thing
to look at for any new target: if the sums agree, the target has no blind spot of
this kind, whatever its device mappings look like.
