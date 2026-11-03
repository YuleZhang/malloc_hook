# Device memory probe（独立设备内存探针）

\`alloc_hook_probe\` 是一个**可直接部署的可执行程序**，独立于 \`liballoc_hook.so\`。它回答的是：

1. 目标 Android / OHOS / 嵌入式 Linux 设备有哪些可观察的内存分配 UAPI 和 kernel 观测接口？
2. 对**实际存在的 UAPI**执行最小的分配、共享和释放序列后，各阶段 \`fdinfo\` 与可选的 kernel 原始计数如何变化？
3. 哪些结论是设备原始数据证明的，哪些只是一次 syscall 成功？

本工具**不是** FD/ION 生命周期模拟器；不会 hook \`fork/dup/close\`，也不会为 malloc_hook 追加新的全局锁和状态管理。它不推断 \`dma_buf inode == backing allocation identity\`。

## 编译与发布

默认 \`MALLOC_HOOK_BUILD_PROBE=ON\`。

- Android：原仓库 \`build_android.sh\` 的 \`ninja install\` 同时安装 \`out/bin/alloc_hook_probe\`。该脚本后续的设备自动测试仍需原有 \`alloc_hook_test\` 环境；只编译或手工安装 probe 可直接使用 CMake。
- OHOS：\`build_ohos.sh\` 同样在 \`out/bin/\` 安装独立 probe。
- glibc Linux / embedded Linux：\`build_linux.sh arm64\` 或 \`build_linux.sh host\` 分别发布 \`out/linux-arm64/bin/alloc_hook_probe\` / \`out/linux-host/bin/alloc_hook_probe\`。
- 只编译 probe（不构建 hook 依赖）：\`cmake -S probe -B build-probe -G Ninja && cmake --build build-probe\`。跨平台时使用对应 NDK / aarch64 交叉工具链。

禁用：\`-DMALLOC_HOOK_BUILD_PROBE=OFF\`。

## 第一步：只读能力探测

\`\`\`bash
adb push out/bin/alloc_hook_probe /data/local/tmp/
adb shell chmod 755 /data/local/tmp/alloc_hook_probe
adb shell /data/local/tmp/alloc_hook_probe inspect \
  > device-capability.jsonl
\`\`\`

\`inspect\` **默认不分配 DMA 内存，也不执行设备 ioctl**，输出每行一个 JSON：

- \`platform\`：内核版本、架构、UID、page size、Android SDK（如适用）。
- \`compiled_abi\`：编译时 ION/DMA-HEAP ioctl 请求号和结构体大小。**不代表目标内核支持该 ABI**。
- \`capability\`：\`/dev/ion\`、\`/dev/dma_heap/*\`、\`/proc/self/fdinfo\`、DMA-BUF/ION debugfs/sysfs 的存在性及读取权限。
- \`limitation\`：明确观测边界。

节点存在、\`access(R_OK)\` 成功不代表 alloc ioctl 一定成功；设备是否可执行必须在下一步确认。缺失的 debugfs / sysfs 信息为 **unknown**，不是 0。

## 第二步：用户明确授权后运行最小实验

\`run\` 才执行真实设备 ioctl。默认一次 16 MiB，允许 1B–64 MiB；设备内存较紧张时请调小 \`--bytes\`。每个 case 单独分配、清理资源，避免把多个实验混在同一内存周期。

### DMA-HEAP（Linux、现代 Android/OHOS）

\`\`\`bash
./alloc_hook_probe run --backend dma-heap --heap system \
  --bytes 16777216 --output /data/local/tmp/dma-heap.jsonl
\`\`\`

执行 \`direct\`（ALLOC → CLOSE）与 \`mapping\`（ALLOC → MMAP → CLOSE → MUNMAP），每一步采集当前 fdinfo 和时间戳。

### Legacy ION（仅设备确实支持时）

\`\`\`bash
./alloc_hook_probe run --backend legacy-ion --ion-heap-mask 0x1 \
  --bytes 16777216 --cases A,B,C,D,E,F \
  --output /data/local/tmp/legacy-ion.jsonl
\`\`\`

\`--ion-heap-mask\` **必须根据目标设备实际 heap 配置明确提供**，probe 不猜测，不通过试遍所有掩码来探测。A–F 的确切实验为：

| Case | 顺序 | 验证的假设 |
| --- | --- | --- |
| A | ALLOC → SHARE → FREE → CLOSE(export FD) | handle 释放与 export FD 的独立性 |
| B | ALLOC → SHARE → SHARE → FREE → CLOSE(fd1) → CLOSE(fd2) | 两次 export 是否是两个独立 DMA-BUF 对象，以及 backing 是否共享 |
| C | ALLOC → SHARE → IMPORT → SHARE(imported handle) → FREE(各引用) → CLOSE(各 FD) | IMPORT/再导出是否形成新 backing allocation |
| D | ALLOC → SHARE → CLOSE(fd) → FREE | 顺序交换后 backing 释放行为 |
| E | ALLOC → SHARE → MMAP → CLOSE(fd) → MUNMAP → FREE | mmap 存活期是否独立于 FD |
| F | ALLOC → SHARE → CLOSE(ION client) → CLOSE(export FD) | client 关闭后 export 是否仍保持资源 |

IMPORT 和 FREE 的重复引用行为依赖实际 vendor ION 驱动；probe 只记录 syscall 返回结果，不用自己维护模拟的 handle state。出错时尽力回收测试程序持有的 FD/handle。

### Modern ION（仅设备确实支持时）

\`\`\`bash
./alloc_hook_probe run --backend modern-ion --ion-heap-mask 0x1 \
  --bytes 16777216
\`\`\`

与 DMA-HEAP 一样执行 \`direct\` / \`mapping\`，但使用 modern ION 的直接 FD 分配 ioctl。

### Auto 模式

\`run --backend auto\` 只自动选择**明确指定且已枚举**的 DMA-HEAP 节点（默认 \`system\`）；不存在时，如果明确传入 \`--ion-heap-mask\`，才尝试 legacy ION。不会凭设备名或 Android 版本猜 ABI。

## 第三步：独立 kernel oracle

可选地将一个**可读取的内核计数文件**提供给 probe，每个实验阶段将读取其原始内容（单次最多 8 KiB，报告截断状态），例如：

\`\`\`bash
./alloc_hook_probe run --backend dma-heap --heap system --bytes 4096000 \
  --oracle /sys/kernel/debug/dma_buf/bufinfo \
  --output /data/local/tmp/dma-evidence.jsonl
\`\`\`

该文件可能需要 root / debugfs mount / SELinux 策略；不要假设所有设备都有。也可以对目标内核中实际存在的 ION heap 计数文件使用 \`--oracle\`。如果该文件是目录或不可读，输出 \`oracle_readable=false\` 及 errno，不伪造数据。

推荐同时使用 **Perfetto/ftrace 中的 ION heap grow/shrink、dma-buf trace 以及原始 kernel source**，按实验时间戳关联；这些工具不由本 probe 冒充实现，需用户根据目标设备配置。

\`sample\` 字段：\`case\`、\`stage\`、\`monotonic_ns\`、\`result\`、\`errno\`、\`dma_fd\`、\`ion_handle\`、原始 \`fdinfo_raw\`、可选 \`oracle_raw\`。输出仅为本次 probe 产生的数据。只读快照容量有限，可能无法覆盖一个繁忙系统上所有 DMA-BUF。

\`case_end.status=pass\` 的含义**仅为 UAPI 序列成功执行**，不是“内核 backing allocation 已被释放”的证明；\`backing_allocation_independently_verified\` 始终为 \`false\`，需另行对照 kernel oracle 和源码。不要把导出 FD inode 个数等同于 backing allocation 个数。

退出码：\`0\` 至少有一个实验成功且没有失败；\`1\` 至少一项真实 UAPI 操作失败；\`2\` 使用参数错误；\`3\` 目标不支持、权限不足或没有可运行的 case。

## 发布与后续

本 PR 的职责止于**探测器、真实 UAPI 最小实验、原始结构化证据与发布**。它不修改 \`malloc_hook\` 的追踪模型，也不依赖 PR #35。后续再依据设备 A–F 的 trace 证明 backing identity 与 release 语义，并决定是否需要调整 \`DmaLifecycle\` / legacy ION 路径。

首次在 Android/OHOS/嵌入式 Linux 真机运行前，请记录内核分支、heap mask 和可访问的 oracle 路径；这些在不同厂商设备上可能不同。
