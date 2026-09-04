# Runtime gate for the observe-only probe: what an LD_PRELOAD'ed run actually
# costs the target has to follow what it produces.
#
# Two runs of the same workload with the same library. The first configures the
# footprint sampler only and must produce the observed peaks on stderr and no
# report file; the second also configures a peak report and must produce the
# report, with no probe line. A regression that made the probe track allocations,
# or made a configured report silently fall back to the probe, fails here rather
# than on a device.
if(NOT DEFINED LIBRARY OR NOT DEFINED WORKLOAD OR NOT DEFINED TEST_BINARY_DIR)
  message(FATAL_ERROR "LIBRARY, WORKLOAD, and TEST_BINARY_DIR are required")
endif()

file(REMOVE_RECURSE "${TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${TEST_BINARY_DIR}")

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
          "LD_PRELOAD=${LIBRARY}"
          "ALLOC_HOOK_PEAK_SAMPLE_MS=5"
          "ALLOC_HOOK_DUMP_PREFIX=${TEST_BINARY_DIR}/probe"
          "${WORKLOAD}"
  RESULT_VARIABLE probe_result
  OUTPUT_VARIABLE probe_stdout
  ERROR_VARIABLE probe_stderr)

if(NOT probe_result EQUAL 0)
  message(FATAL_ERROR
    "Observe-only run must not disturb the workload, exited ${probe_result}:\n${probe_stderr}")
endif()
foreach(expected
    "observe_only probe (at_exit)"
    "Memory Usage Summary"
    "  RSS Max (sampling):"
    "  DMA+RSS+GPU mmap Max (sampling):"
    "  Sampling Period:"
    "observed_peak(max_of_sum):"
    "observed_sampler: interval_ms=5")
  string(FIND "${probe_stderr}" "${expected}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "Observe-only run did not report '${expected}':\n${probe_stderr}")
  endif()
endforeach()
# Exactly once. The report is reachable from atexit() and from this library's
# destructor, and a host libc runs both -- the atexit registration exists because
# Bionic runs neither at process exit.
string(REGEX MATCHALL "observe_only probe \\(at_exit\\)" exit_lines "${probe_stderr}")
list(LENGTH exit_lines exit_line_count)
if(NOT exit_line_count EQUAL 1)
  message(FATAL_ERROR
    "Observe-only run reported at exit ${exit_line_count} times, expected once:\n${probe_stderr}")
endif()
# The whole point of the mode: nothing was tracked, so there is no report to
# write, and the run paid for no stack capture to fill one.
file(GLOB probe_reports "${TEST_BINARY_DIR}/probe.*")
if(probe_reports)
  message(FATAL_ERROR
    "Observe-only run wrote a report it has no allocation data for: ${probe_reports}")
endif()

# The step is what turns the same interval into a report, so it has to be shown
# doing that and not only shown being absent.
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
          "LD_PRELOAD=${LIBRARY}"
          "ALLOC_HOOK_PEAK_SAMPLE_MS=5"
          "DUMP_PEAK_STEP_MB=8"
          "ALLOC_HOOK_DUMP_PREFIX=${TEST_BINARY_DIR}/chase"
          "${WORKLOAD}"
  RESULT_VARIABLE chase_result
  OUTPUT_VARIABLE chase_stdout
  ERROR_VARIABLE chase_stderr)
if(NOT chase_result EQUAL 0)
  message(FATAL_ERROR
    "Peak-chasing run failed, exited ${chase_result}:\n${chase_stderr}")
endif()
string(FIND "${chase_stderr}" "observe_only probe" chase_probe_position)
if(NOT chase_probe_position EQUAL -1)
  message(FATAL_ERROR
    "An interval with a step asks for a report, not the probe:\n${chase_stderr}")
endif()
file(GLOB chase_reports "${TEST_BINARY_DIR}/chase.exit.*")
if(NOT chase_reports)
  message(FATAL_ERROR "Peak-chasing run wrote no exit report")
endif()

# 0 is each variable's own off switch, so a run that spells both of them 0 has
# asked for no report and must still get the probe. A regression that read 0 as
# "on, without a limit" would write a report here.
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
          "LD_PRELOAD=${LIBRARY}"
          "ALLOC_HOOK_PEAK_SAMPLE_MS=5"
          "DUMP_PEAK_VALUE_MB=0"
          "DUMP_PEAK_STEP_MB=0"
          "ALLOC_HOOK_DUMP_PREFIX=${TEST_BINARY_DIR}/zeros"
          "${WORKLOAD}"
  RESULT_VARIABLE zeros_result
  OUTPUT_VARIABLE zeros_stdout
  ERROR_VARIABLE zeros_stderr)
if(NOT zeros_result EQUAL 0)
  message(FATAL_ERROR "Run with both switches off failed: ${zeros_stderr}")
endif()
string(FIND "${zeros_stderr}" "Memory Usage Summary" zeros_probe_position)
if(zeros_probe_position EQUAL -1)
  message(FATAL_ERROR
    "A floor and a step of 0 turn both reports off, so the probe must run:\n${zeros_stderr}")
endif()
file(GLOB zeros_reports "${TEST_BINARY_DIR}/zeros.*")
if(zeros_reports)
  message(FATAL_ERROR "0 must mean off, but a report was written: ${zeros_reports}")
endif()

execute_process(
  COMMAND ${CMAKE_COMMAND} -E env
          "LD_PRELOAD=${LIBRARY}"
          "ALLOC_HOOK_PEAK_SAMPLE_MS=5"
          "DUMP_PEAK_VALUE_MB=8"
          "ALLOC_HOOK_DUMP_PREFIX=${TEST_BINARY_DIR}/report"
          "${WORKLOAD}"
  RESULT_VARIABLE report_result
  OUTPUT_VARIABLE report_stdout
  ERROR_VARIABLE report_stderr)

if(NOT report_result EQUAL 0)
  message(FATAL_ERROR
    "Tracking run failed, exited ${report_result}:\n${report_stderr}")
endif()
string(FIND "${report_stderr}" "observe_only probe" probe_line_position)
if(NOT probe_line_position EQUAL -1)
  message(FATAL_ERROR
    "A configured report must not fall back to the probe:\n${report_stderr}")
endif()
file(GLOB reports "${TEST_BINARY_DIR}/report.exit.*")
if(NOT reports)
  message(FATAL_ERROR "Tracking run wrote no exit report to ${TEST_BINARY_DIR}")
endif()
list(GET reports 0 report_file)
file(READ "${report_file}" report_contents)
foreach(expected
    "peak_criterion: observed_host_rss_plus_dma_plus_gpu"
    "rss_breakdown(at_peak)"
    "alloc_size")
  string(FIND "${report_contents}" "${expected}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR
      "Tracking run's report is missing '${expected}': ${report_file}")
  endif()
endforeach()
