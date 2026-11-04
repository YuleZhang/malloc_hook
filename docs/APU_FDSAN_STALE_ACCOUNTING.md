# X6765 APU DMA stale accounting: scoped fix for #35

## Root cause and evidence

Old malloc_hook recorded DMA bytes by numeric fd. APU cleanup uses
`munmap(addr)` then Bionic `android_fdsan_close_with_tag(fd, tag)`.
The latter internally uses its own close and bypasses the public close hook.
Removing an address mapping does not remove an fd-keyed allocation record.

Controlled diagnostic A/B (with the real Bionic tag checks preserved):

| Metric | Baseline | Tagged-close shim |
|---|---:|---:|
| APU logical residue after release (mean) | 78.38 MiB | 0 |
| Stale APU entries after release | 24–28 | 0 |
| APU logical residue at exit | 113.54 MiB | 0 |
| Mali DMA | ~9.21 MiB | ~9.21 MiB |
| HAIO DMA peak | 117.145 MiB | 117.145 MiB |
| Functional checks | PASS | PASS |

These A/B results strongly implicate missing tagged-close bookkeeping,
**not** the kernel's backing-memory reclamation time. A precise inode/kprobe
oracle independently found at most three 4 MiB APU_CMDBUF_POOL exports
crossing the release marker, followed by last dma_buf_put at +3.334 to
+3.391 ms through mdw_mem_pool_destroy -> mdw_drv_close -> __fput -> do_exit.
Previous 14-inode / 26.145 MiB observations were contaminated by next-round
workload. Not all allocations with the libapu_mdw memAlloc -> mmap call stack
are command-buffer-pool allocations.

The listed size buckets in one 83.398 MiB dump total ~79.73 MiB, so that
list must not be represented as a complete breakdown. FD reuse is a
plausible source of nonmonotonic stale records, not a directly observed
collision in this particular run.

## Scope

- Interpose Bionic android_fdsan_close_with_tag, calling the **real** Bionic
  function with its original ownership tag; do not bypass tag validation.
- Route normal and tagged close through the same debug_close_with path.
- Serialize observed DMA-FD producing ioctls, mmap discovery and close
  while transferring fd-keyed bookkeeping; do not lock arbitrary vendor ioctls.
- Replace a stale numeric-fd entry when a successful allocation/export
  ioctl guarantees that the kernel returned a new descriptor.
- Preserve EBADF state; do not retry close on EINTR/EIO.
- Keep existing Host/mmap/legacy ION heuristics unchanged; no extra
  fork, dup, fcntl, ION-client or global object lifecycle framework.

## Limitations and device acceptance

The mutex protects only observed producer/consumer hook paths, not
uninterposed raw syscalls, unrelated open/SCM_RIGHTS or vendor-internal FD
creation. This is not a full FD identity/generation or backing allocation
model. Legacy ION multiple exports may still be counted separately.

Host test checks callback forwarding, tagged context, EBADF preservation
and EINTR late-close behavior. Android must independently verify:
1. A valid tagged descriptor closes exactly once.
2. A mismatched tag still triggers Bionic's configured diagnostics/abort.
3. APU logical residue disappears after release_handle with no peak/Mali
   regression; kernel residency is independently judged by probe/kprobe.

Source: user-provided X6765 A/B and release-marker/kernel trace evidence.
## Android 真机验收工具（#35）

新增可选构建目标 `android_fdsan_device_gate`，独立于 Host CTest，不需要 APU 专有业务库。

```bash
cmake -S . -B build-fdsan-android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release -DMALLOC_HOOK_BUILD_TESTS=OFF \
  -DMALLOC_HOOK_BUILD_FDSAN_DEVICE_GATE=ON
cmake --build build-fdsan-android --target alloc_hook android_fdsan_device_gate
adb push build-fdsan-android/liballoc_hook.so /data/local/tmp/
adb push build-fdsan-android/android_fdsan_device_gate /data/local/tmp/
adb shell 'cd /data/local/tmp && chmod 755 android_fdsan_device_gate && LD_PRELOAD=./liballoc_hook.so ./android_fdsan_device_gate valid'
adb shell 'cd /data/local/tmp && LD_PRELOAD=./liballoc_hook.so ./android_fdsan_device_gate invalid'
adb shell 'cd /data/local/tmp && LD_PRELOAD=./liballoc_hook.so ./android_fdsan_device_gate dma /dev/dma_heap/mtk_mm-uncached'
```

`valid` 强制检查动态符号实际解析到预加载的 `liballoc_hook.so`，并验证合法 tag 关闭 fd。
`invalid` 使用 `fork + exec` 隔离错误 tag 场景，子进程显式开启 fatal，父进程验证子进程因 `SIGABRT` 终止；这样不会把 parent 当成测试失败而退出。
`dma` 直接走真实 DMA-HEAP ioctl，使用三次 `checkpoint` 比较逻辑 DMA live bytes：基线、+4 MiB、回到基线。该测试需要设备允许访问指定 `/dev/dma_heap/...`；不支持时退出码为 77（SKIP），**绝不把 SKIP 当 PASS**。

NDK CI 仅覆盖此测试程序的 Android 交叉编译，不具备 X6765 设备，不能宣称已经完成上述三项设备运行验证。真实业务的 10 轮 APU regression 和 kernel oracle 仍独立验收。