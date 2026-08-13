# liballoc_hook.so

use to get malloc and free backtrace, include dmabuffer by hook `ioctl` and `close`

* how to build

  * ./build_android.sh [armeabi-v7a]
  * ./build_ohos.sh [arm64-v8a|armeabi-v7a|x86_64]
    * set `OHOS_NDK_ROOT` to an OHOS `native` NDK directory, or let the script download the default NDK into `.deps/ohos_ndk/native`
    * output: `out/lib/liballoc_hook.so`

* how to use
  * adb shell mkdir `path`, where `path` is the output path for the unwindstack, default: `/data/local/tmp/trace/`
  * LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=. ls
  * then replace `ls` to you real command
  * 如果要抓 trace 请先创建 trace 的输出目录
  * 使用该工具导致程序运行过慢时，可以指定环境 `BACKTRACE_MIN_SIZE` 值，不记录小内存的堆栈信息

  * how to use on OHOS
  * 构建: `./build_ohos.sh arm64-v8a`
  * OHOS 版本默认导出 heap hooks 以及 `ioctl/close`，用于记录 `DMA_HEAP_IOCTL_ALLOC` 返回的 DMA-BUF fd 及其释放；仍不导出 `mmap/munmap`，避免影响 OpenCL 驱动映射初始化
  * 如需单独验证匿名 `mmap` 调用栈，可使用 `OHOS_ENABLE_MMAP_HOOK=ON ./build_ohos.sh arm64-v8a` 构建 mmap 调试版；该版本只适合小型复现程序，OpenCL pipeline 中导出 `mmap/munmap` 会触发 vendor runtime `SIGTRAP`，不建议用于正式 pipeline 跑图
  * OHOS 默认 `BACKTRACE_MIN_SIZE=40960`，可通过环境变量覆盖；不建议设置为 0，会明显拖慢复杂 pipeline
  * 直接运行命令并抓取: `./run_on_ohos.sh /data/local/tmp/alloc_test ./your_program arg1 arg2`
  * 手动执行时，先推送 `out/lib/liballoc_hook.so`，然后在 OHOS shell 中运行:
    ``` bash
      mkdir -p /data/local/tmp/trace
      export LD_LIBRARY_PATH=.
      export BACKTRACE_MIN_SIZE=40960
      export BACKTRACE_DUMP_SIGNAL=46
      LD_PRELOAD=./liballoc_hook.so ./your_program
    ```
  * Android arm64 可选构建 OpenCL companion probe（仅在需要 API marker 时启用）：
    ```bash
    MALLOC_HOOK_BUILD_TESTS=OFF MALLOC_HOOK_OPENCL_PROBE=ON \
      MALLOC_HOOK_RUN_DEVICE_TEST=OFF ./build_android.sh arm64-v8a
    ```
    这会额外生成 `out/lib/libopencl_probe.so`。当前已完成 Android arm64
    NDK 交叉编译验证；尚未在 Android 真机上完成 FaceSR runtime/marker 验证，
    因而不能把该 probe 的 Android 行为当作已验证结论。真机验证时应使用
    `adb -s <serial>`（或设置 `ANDROID_SERIAL`）明确选择设备，并单独检查
    `MALLOC_HOOK_OPENCL_MARKERS=1` 下 marker 数量、OpenCL 返回码和 workload
    退出码。
  * 外部触发 checkpoint: `kill -46 <pid>`，其中信号值需和 `BACKTRACE_DUMP_SIGNAL` 保持一致；OHOS 上 `33/45` 可能被系统或运行时保留/覆盖，不建议使用

* checkpoint
  * 支持在程序指定位置插入检查点，输出当前时刻的未释放的内存的堆栈信息
  * 第一种方式：使用信号的方式触发堆栈输出，Android 默认信号值为 33，OHOS 默认使用 `46`；也可以用环境变量 `BACKTRACE_DUMP_SIGNAL` 指定固定信号值，trace 文件以当前时间命名
    ``` C++
      #include <unistd.h>
      #include <signal.h>
      #include <errno.h>
      #include <stdlib.h>
      #include <string.h>

      if (kill(getpid(), 33) == -1) {
        fprintf(stderr, "Error in file %s at line %d: %s\n", __FILE__, __LINE__, strerror(errno));
        exit(1);
      }
    ```
  * 第二种方式：在代码中插入调用，支持用户自定义文件名，前提需要用户的可执行程序依赖该项目编译生成的 so, 例如在 cmake 文件中做出下面的修改
    ```
      add_library(hook SHARED IMPORTED)
      set(HOOK_IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/liballoc_hook.so")
      set_target_properties(hook PROPERTIES IMPORTED_LOCATION "${HOOK_IMPORTED_LOCATION}")

      add_executable(xxxx ./code/xxxxx.cpp)
      target_link_libraries(xxxx hook)
    ```
    用户代码需做以下修改
    ``` C++
      extern "C" void checkpoint(const char* file_name);
      int main() {
          size_t size = 200 * 1024 * 1024;
          void* addr_mmap = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
          checkpoint("/data/local/tmp/trace/check_point.1.txt");
          void* addr_malloc = malloc(size * 2);
          
          return 0;
      }
    ```
    如果以 SHARED 方式生成了 liballoc_hook.so 共享库，则需要将 liballoc_hook.so 文件放在可执行程序可读取的目录中，例如
    ``` bash
      adb push /path/liballoc_hook.so ${test_dir}

      adb exec-out "cd ${test_dir}; \
                export LD_LIBRARY_PATH=.; \
                ./xxxx;"
    ```
  * 第三种方式：使用 `dlsym` 打开 checkpoint 符号，并用 `LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=. ls` 的方式执行
  ```c++
      typedef void (*checkpoint_func)(const char*);
      auto checkpoint = (checkpoint_func)dlsym(RTLD_DEFAULT, "checkpoint");
      if (checkpoint) {
        checkpoint("/data/local/tmp/trace/check_point.1.txt");
      }
  ```

* 如何改造自己的被测试程序以便此工具能`有效`采样

  另外在采样过程中，也请务必保证程序处于`停止`状态，常见的做法是在被测试的代码适当位置加上 checkpoint() 或者 kill(getpid(), 33) 以便触发采样，
  下面解释一下什么叫做`适当`位置

  ```
  比如典型的 pipline:
  create_ctx()
  ....
  use_ctx()
  ....
  free_ctx()
  ```

  一般期望是 free_ctx 后应该资源都释放了（除开常驻的外), 我们改造上述 pipline 来加入 checkpoint()/kill, 以便让此工具有采样点

  ```
  for(;;)
  {
  	create_ctx()
  	....
  	use_ctx()
  	....
  	free_ctx()
  	checkpoint() or kill(getpid(), 33)
  }
  ```

  典型的，我们需要让整个 pipline 运行几次分别得到不同运行次数的采样 trace, 当然除了 checkpoint/kill 让程序`暂停`外，我们也可以通过 `gdb` 等调试工具来达到相同的目的。关键在于加的位置，一定是你认为这个点应该释放了资源，比如典型的上面的 free_ctx，一定不要在其他点去进行采样，比如 `use_ctx` 阶段，这个时候本身属于内存用量高峰，即使没有释放也不能说明`泄漏`对吧。

* 抓取峰值步骤
  - 首先运行一次程序，当程序结束时，会输出如下信息
  ```
  +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
  host peak used: 415MB, dma peak used 206MB, total peak used: 619MB
  ```
  - 然后，根据 total peak used 的值，通过环境变量 `DUMP_PEAK_VALUE_MB` 设置 backtrace_dump_peak_val_ 的值，通常要小于 total peak used 50MB 左右，设置方式如下
  ```
  export DUMP_PEAK_VALUE_MB=xxx

  或者

  DUMP_PEAK_VALUE_MB=xxx LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=. ls
  ```
  - DUMP_PEAK_VALUE_MB 的单位默认为 MB
  - `DUMP_PEAK_STEP_MB` 控制峰值快照的最小增长间隔，默认 64MB；设置 `DUMP_PEAK_VALUE_MB` 后，工具会在首次超过阈值时保存峰值快照，之后只有峰值再次增长超过该间隔才重建快照，避免在运行时反复抓取和聚合堆栈导致卡住。
  - OHOS 上设置 `MALLOC_HOOK_TRACE_PEAK=1` 时，每次峰值快照提交都会写入 `/sys/kernel/tracing/trace_marker`。Perfetto/HiTrace 中可搜索 `malloc_hook_peak_snapshot`，并查看 `malloc_hook_peak_total_bytes`、`malloc_hook_peak_host_bytes`、`malloc_hook_peak_dma_bytes` counter；最后一个 snapshot 对应退出时写入 `backtrace_heap.exit.*.txt` 的峰值快照。
  - 设置 `MALLOC_HOOK_SAMPLE_PEAK=1` 后，支持由采样器调用
    `malloc_hook_record_sample_peak(epoch_ms, dma_bytes, rss_bytes)` 保存同一采样
    事件的候选快照。`MALLOC_HOOK_SAMPLE_PEAK_THRESHOLD_MB` 控制首次抓取
    阈值，`MALLOC_HOOK_SAMPLE_PEAK_STEP_MB` 控制后续新高的最小增量，
    `MALLOC_HOOK_SAMPLE_PEAK_DIR` 控制输出目录。每张快照包含 smaps、
    smaps_rollup、ION、hook live ledger、采集起止时间以及可用时的 OpenCL
    requested-bytes ledger；默认关闭。
  - 上述两套峰值机制彼此独立：
    - `DUMP_PEAK_VALUE_MB` / `DUMP_PEAK_STEP_MB` 跟踪 malloc_hook 自身维护的
      `host live + DMA live`，用于得到分配调用栈峰值。
    - `MALLOC_HOOK_SAMPLE_PEAK_*` 不主动采样 RSS。HAIO 或其它采样器必须在
      取得同一行 `dma_cur` 和 `rss_cur` 后调用
      `malloc_hook_record_sample_peak(epoch_ms, dma_bytes, rss_bytes)`；hook 再按
      `DMA + RSS` 判断是否保存 smaps/PSS/ION/live-ledger 快照。
    - 两套阈值可同时启用，互不覆盖。前者可能生成
      `backtrace_heap*.txt` 和 `malloc_hook_peak_snapshot` trace marker，后者生成
      `MALLOC_HOOK_SAMPLE_PEAK_DIR` 下的目录快照。分析时必须按各自 accounting
      domain 展示，不能把 hook live 峰值与 HAIO DMA+RSS 求和。
    - 如只需要 DMA+RSS/PSS 同时刻快照，可不设置 `DUMP_PEAK_VALUE_MB`，仅开启
      `MALLOC_HOOK_SAMPLE_PEAK=1` 并由采样器调用导出接口。当前 live ledger 中
      的调用栈由每个仍存活 allocation 的引用自动保留；需要记录新分配的调用栈
      时仍应合理设置 `BACKTRACE_MIN_SIZE`。不需要为 sampled mode 额外开启
      `RECORD_MEMORY_PEAK`。
    - 两套机制同时启用时不会覆盖彼此：`DUMP_PEAK_VALUE_MB` 仅控制
      malloc_hook 的 host+DMA peak；`MALLOC_HOOK_SAMPLE_PEAK_*` 仅控制采样器传入
      的同一行 DMA+RSS 候选。若同时设置同名的 step/threshold，应分别使用各自
      前缀；不要把两个 accounting domain 的数值相加。
  - OHOS 构建可选启用 `-DMALLOC_HOOK_OPENCL_PROBE=ON`。运行时再设置
    `MALLOC_HOOK_OPENCL_MARKERS=1`，malloc_hook 会拦截 FaceSR 内置 OpenCL
    stub 通过 `dlsym()` 解析出的关键 API（context/queue、buffer/image、
    program build、kernel、NDRange、release），向 trace_marker 写入 API、
    对象句柄、请求字节数、live requested bytes 和 dispatch 维度。默认关闭，
    不导出 OHOS mmap hook；可用 `MALLOC_HOOK_OPENCL_MARKER_PATH=/data/local/tmp/...`
    写入独立 marker 文件。
  - 设置 `MALLOC_HOOK_OPENCL_PROCESS_RSS=1` 后，每个 OpenCL marker 额外记录
    `/proc/self/status` 的 `VmRSS`，用于把 program/kernel 生命周期与进程 RSS
    高水位对齐。该读取默认关闭，避免给正式 pipeline 增加开销。

* 将 HAIO CSV 合并到 OHOS trace
  - `scripts/merge_trace_csv.py` 会保留原始 ftrace 中的
    `malloc_hook_peak_snapshot`、`malloc_hook_ocl` 和其它
    `tracing_mark_write` 事件，并把 CSV 数值列作为 Perfetto counter tracks
    合并进去。
  - 输入原始 ftrace：
  ```
  python3 scripts/merge_trace_csv.py \
      --trace facesr_raw.ftrace \
      --csv mem_use_info_process_1234.csv \
      --pid 1234 \
      --output facesr_memory_overlay.html
  ```
  - 输入已经包装好的 systrace HTML：
  ```
  python3 scripts/merge_trace_csv.py \
      --trace facesr_raw.html \
      --csv mem_use_info_process_1234.csv \
      --pid 1234 \
      --output facesr_memory_overlay.html
  ```
  - 默认合并 CSV 中除 `time` 外的全部数值列。底层脚本也可单独使用：
  ```
  python3 scripts/merge_csv_to_perfetto.py --list-columns --csv memory.csv
  python3 scripts/merge_csv_to_perfetto.py \
      --trace facesr_raw.html --csv memory.csv \
      --column dma_cur --column rss_cur \
      --html-pid 1234 --output selected_memory_overlay.html
  ```
  - 脚本会验证每条 counter track 的事件数是否等于 CSV 行数，并打印 trace
    与 overlay 时间范围、descriptor 数和 counter event 数。

* 内存泄露分析步骤
  - 利用 cheakpoint 机制执行两次程序，并对两次的内存调用堆栈输出进行对比，分析内存调用的增量，此时的内存调用是以时间排序，可以从后向前对比
  ``` c++
    void test() {
      ....
    }

    int main() {
       for (int i = 0; i < 2; ++i) {
        test();
        kill(getpid(), 33);
       }
    }

  ```

* 在线 trace 抓取
  以相机程序为例：
  - 首先打开相机，使用 top / ps / pidof / pgrep 查看相机服务进程 id, 相机服务名称一般为 camerahalserver
  - 查看相机服务的 cmdline 信息, `cat /proc/<pid>/cmdline`
  - 查看启动命令
    - 使用 `find -L /system -name "*.rc" | xargs grep "CMDLINE"` 搜索相机服务启动命令, "CMDLINE" 为第二步的输出
      - 该步骤输出为: "/path/xx.rc", 例如, "/system/vendor/etc/init/camerahalserver.rc"
    - 使用 `cat /path/xx.rc` 查看启动命令所需的参数，"/path/xx.rc" 为上一步骤的输出, rc 文件的格式如下
      ```
      service <name> <pathname> [ <argument> ]*
       <option>
       <option>
      ```
    如下所示, 启动命令为 /vendor/bin/hw/camerahalserver [或者, `start camerahalserver`], 一般需要在根目录启动服务
      ```
      service camerahalserver /vendor/bin/hw/camerahalserver
        class main
        user cameraserver
        group audio camera input drmrpc sdcard_rw system media graphics
        ioprio rt 4
        capabilities SYS_NICE
        task_profiles CameraServiceCapacity MaxPerformance
      ```
  - 使用 `stop camerahalserver` 停止相机服务
  - 根据第三步的 rc 文件的输出, 重新启动服务, 此时, 可以增加 LD_PRELOAD 抓取堆栈, 如果相机服务卡顿严重，建议使用 BACKTRACE_MIN_SIZE 过滤小于 1KB 的内存
    ```
    LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=/path /vendor/bin/hw/camerahalserver
    ```
  - 在拍照完后, Android 使用 `kill -33 <pid>` 输出当前时刻的堆栈；OHOS 使用 `kill -46 <pid>`
    - 一般返回桌面，等待几秒再调用 kill 命令发送信号，保证相机程序申请的内存已经释放，防止统计错误

* 配置参数意义
  - `backtrace_dump_on_exit_`: 程序退出时，打印堆栈
  - `backtrace_frames_`: 抓取堆栈的最大深度，默认 128
  - `backtrace_dump_prefix_`: 输出堆栈信息的文件名前缀
  - `BACKTRACE_SPECIFIC_SIZES`: 分配内存时，是否抓取指定 alloc size 的堆栈信息
  - `backtrace_min_size_bytes_`: 开启 BACKTRACE_SPECIFIC_SIZES 标志时，抓取 alloc size 大于该值的堆栈信息
  - `backtrace_max_size_bytes_`: 开启 BACKTRACE_SPECIFIC_SIZES 标志时，抓取 alloc size 小于该值的堆栈信息
  - `RECORD_MEMORY_PEAK`: 是否抓取峰值时刻的堆栈信息
  - `backtrace_dump_peak_val_`: 当峰值大于该值时，记录峰值时刻的堆栈
  - `DUMP_ON_SINGAL`: 开启 checkpoint 信号机制
  - `backtrace_dump_signal_`: checkpoint 信号机制的信号值，默认 33
  - `DUMP_PEAK_VALUE_MB`：环境变量，单位: MB，当内存峰值大于该值时记录峰值内存
  - `DUMP_PEAK_STEP_MB`：环境变量，单位: MB，控制峰值快照重建间隔，默认 64MB
  - `BACKTRACE_MIN_SIZE`：环境变量，单位: Byte，当申请内存的 size 大于该值时，才抓取堆栈信息
  - `配置文件位于 backtrace/src/Config.cpp, 可在该文件中修改上述参数`
