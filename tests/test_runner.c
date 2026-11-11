/**
 * @file test_runner.c
 * @brief Comprehensive automated unit and integration test runner for CRR.
 *
 * This test suite executes 41 distinct assertions across 11 test suites:
 * 1. Unit Tests:
 *    - Options initialization defaults.
 *    - Byte formatting thresholds (B, KiB, MiB, GiB).
 *    - Time formatting thresholds (ms, s, m).
 *    - Statistics math verification (sample mean, Bessel-corrected standard deviation).
 *    - JSON and CSV serialization formatting.
 *
 * 2. Integration and Systems Tests:
 *    - Exit code propagation (testing status 0 and non-zero code 42).
 *    - Wall-clock measurement accuracy (sleep workloads).
 *    - User CPU and utilization calculation (cpu_burn workload).
 *    - Peak Resident Set Size (Peak Working Set) and page fault detection (mem_burn).
 *    - Timeout enforcement and process tree termination (sleep_delay with threshold).
 *    - Standard output interception and pipe drainage.
 *    - Windows Job Object multi-process child tree tracking.
 */

#include "crr.h"
#include "crr_executor.h"
#include "crr_formatter.h"
#include "crr_stats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <ctype.h>

static int total_tests = 0;
static int passed_tests = 0;
static int failed_tests = 0;

/**
 * @brief Macro to record and log test results.
 */
#define TEST_ASSERT(cond, msg) do { \
    total_tests++; \
    if (cond) { \
        passed_tests++; \
        printf("  [PASS] %s\n", msg); \
    } else { \
        failed_tests++; \
        printf("  [FAIL] %s (Line %d)\n", msg, __LINE__); \
    } \
} while (0)

/* ========================================================================= */
/* UNIT TEST SUITES                                                          */
/* ========================================================================= */

/**
 * @brief Validates that options initialization populates default values correctly.
 */
static void test_options_init(void) {
    printf("[TEST SUITE] Options Initialization\n");
    crr_options_t opts;
    crr_options_init(&opts);

    TEST_ASSERT(opts.timeout_ms == 0, "Default timeout is 0 (infinite)");
    TEST_ASSERT(opts.repeat_count == 1, "Default repeat count is 1");
    TEST_ASSERT(opts.track_children == true, "Child tracking is enabled by default");
    TEST_ASSERT(opts.format == CRR_FORMAT_HUMAN, "Default format is human");
    TEST_ASSERT(opts.capture_output == false, "Default capture output is false");
}

/**
 * @brief Validates human-readable formatting of byte scales and time intervals.
 */
static void test_byte_and_time_formatters(void) {
    printf("[TEST SUITE] Formatters (Bytes & Time)\n");
    char buf[64];

    /* Byte formatting tests */
    crr_format_bytes(500, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "500 B") != NULL, "500 bytes formats to '500 B'");

    crr_format_bytes(1024, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "1.00 KiB") != NULL, "1024 bytes formats to '1.00 KiB'");

    crr_format_bytes(1048576, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "1.00 MiB") != NULL, "1048576 bytes formats to '1.00 MiB'");

    crr_format_bytes(1073741824ULL * 2ULL, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "2.00 GiB") != NULL, "2 GiB formats properly");

    /* Time formatting tests */
    crr_format_time(0.005, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "ms") != NULL, "0.005 s formats as milliseconds");

    crr_format_time(4.25, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "4.2500 s") != NULL, "4.25 s formats as seconds");

    crr_format_time(75.5, buf, sizeof(buf));
    TEST_ASSERT(strstr(buf, "1m") != NULL, "75.5 s formats with minutes");
}

/**
 * @brief Validates statistical computations against exact mathematical ground truth.
 */
static void test_stats_math(void) {
    printf("[TEST SUITE] Statistics Calculations\n");
    crr_metrics_t runs[3];
    memset(runs, 0, sizeof(runs));

    /* Populate 3 synthetic runs with known values: {1.0, 2.0, 3.0} */
    runs[0].wall_time_sec = 1.0; runs[0].max_rss_bytes = 1000; runs[0].status_code = CRR_SUCCESS;
    runs[1].wall_time_sec = 2.0; runs[1].max_rss_bytes = 2000; runs[1].status_code = CRR_SUCCESS;
    runs[2].wall_time_sec = 3.0; runs[2].max_rss_bytes = 3000; runs[2].status_code = CRR_SUCCESS;

    crr_stats_summary_t sum;
    crr_compute_stats(runs, 3, &sum);

    TEST_ASSERT(sum.runs_count == 3, "Stats run count is 3");
    TEST_ASSERT(fabs(sum.wall_time.mean - 2.0) < 0.0001, "Wall time mean is 2.0");
    TEST_ASSERT(fabs(sum.wall_time.min - 1.0) < 0.0001, "Wall time min is 1.0");
    TEST_ASSERT(fabs(sum.wall_time.max - 3.0) < 0.0001, "Wall time max is 3.0");
    TEST_ASSERT(fabs(sum.wall_time.stddev - 1.0) < 0.0001, "Wall time stddev is 1.0");
    TEST_ASSERT(fabs(sum.max_rss_bytes.mean - 2000.0) < 0.0001, "Max RSS mean is 2000");
}

/**
 * @brief Validates JSON and CSV serialization formatting.
 */
static void test_json_and_csv_serialization(void) {
    printf("[TEST SUITE] JSON & CSV Serialization\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.command = "sample_cmd";

    crr_metrics_t m;
    memset(&m, 0, sizeof(m));
    m.wall_time_sec = 1.234;
    m.user_cpu_time_sec = 0.5;
    m.kernel_cpu_time_sec = 0.1;
    m.total_cpu_time_sec = 0.6;
    m.cpu_utilization_pct = 48.62;
    m.max_rss_bytes = 1024 * 1024 * 16;
    m.page_faults = 400;

    char json_buf[4096];
    int jres = crr_format_json(&m, &opts, json_buf, sizeof(json_buf));
    TEST_ASSERT(jres == 0, "JSON format succeeds");
    TEST_ASSERT(strstr(json_buf, "\"wall_time_sec\": 1.234000") != NULL, "JSON contains wall_time_sec");
    TEST_ASSERT(strstr(json_buf, "\"max_rss_bytes\": 16777216") != NULL, "JSON contains max_rss_bytes");

    char csv_buf[4096];
    int cres = crr_format_csv(&m, &opts, csv_buf, sizeof(csv_buf), true);
    TEST_ASSERT(cres == 0, "CSV format succeeds");
    TEST_ASSERT(strstr(csv_buf, "wall_time_sec") != NULL, "CSV contains header");
    TEST_ASSERT(strstr(csv_buf, "16777216") != NULL, "CSV contains RSS value");
}

/* ========================================================================= */
/* INTEGRATION & SYSTEMS TEST SUITES                                         */
/* ========================================================================= */

/**
 * @brief Validates exit code capture for both successful and failing processes.
 */
static void test_exit_codes(void) {
    printf("[TEST SUITE] Exit Code Propagation\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.quiet = true;

    crr_metrics_t m0;
    opts.command = "bin\\fail_crash.exe 0";
    crr_execute(&opts, &m0, NULL);
    TEST_ASSERT(m0.exit_code == 0, "Exit code 0 correctly reported");

    crr_metrics_t m42;
    opts.command = "bin\\fail_crash.exe 42";
    crr_execute(&opts, &m42, NULL);
    TEST_ASSERT(m42.exit_code == 42, "Exit code 42 correctly reported");
}

/**
 * @brief Validates elapsed wall-clock time measurement and verifies sleeping processes consume near-zero CPU.
 */
static void test_wall_clock_measurement(void) {
    printf("[TEST SUITE] Wall-Clock Time Measurement\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.quiet = true;
    opts.command = "bin\\sleep_delay.exe 350";

    crr_metrics_t m;
    crr_execute(&opts, &m, NULL);

    TEST_ASSERT(m.wall_time_sec >= 0.30, "Wall-clock elapsed time >= 0.30 s");
    TEST_ASSERT(m.user_cpu_time_sec < 0.15, "Sleeping process uses minimal user CPU (< 0.15 s)");
    TEST_ASSERT(m.cpu_utilization_pct < 35.0, "Sleeping process has low CPU utilization");
}

/**
 * @brief Validates CPU time accumulation and CPU utilization calculation under heavy computational loads.
 */
static void test_cpu_time_measurement(void) {
    printf("[TEST SUITE] CPU Time & Utilization Measurement\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.quiet = true;
    opts.command = "bin\\cpu_burn.exe 200";

    crr_metrics_t m;
    crr_execute(&opts, &m, NULL);

    TEST_ASSERT(m.total_cpu_time_sec >= 0.10, "CPU burn recorded significant CPU time (>= 0.10 s)");
    TEST_ASSERT(m.cpu_utilization_pct > 30.0, "CPU burn recorded significant CPU utilization (> 30%)");
}

/**
 * @brief Validates Peak Working Set (Max RSS) memory measurement and page fault counters.
 */
static void test_max_rss_measurement(void) {
    printf("[TEST SUITE] Maximum Resident Set Size (Peak Working Set)\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.quiet = true;
    opts.command = "bin\\mem_burn.exe 25";

    crr_metrics_t m;
    crr_execute(&opts, &m, NULL);

    uint64_t twenty_mib = 20ULL * 1024ULL * 1024ULL;
    TEST_ASSERT(m.max_rss_bytes >= twenty_mib, "Max RSS recorded >= 20 MiB for a 25 MiB committed allocation");
    TEST_ASSERT(m.page_faults > 1000, "Page faults counted when committing memory (> 1000)");
}

/**
 * @brief Validates automatic process termination upon exceeding a configured timeout limit.
 */
static void test_timeout_enforcement(void) {
    printf("[TEST SUITE] Timeout Enforcement\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.quiet = true;
    opts.command = "bin\\sleep_delay.exe 3000";
    opts.timeout_ms = 400; /* 400ms timeout on a 3-second sleep */

    crr_metrics_t m;
    int res = crr_execute(&opts, &m, NULL);

    TEST_ASSERT(m.timed_out == true, "Timeout flag set when process exceeds threshold");
    TEST_ASSERT(res == CRR_ERR_TIMED_OUT, "Return status is CRR_ERR_TIMED_OUT");
    TEST_ASSERT(m.wall_time_sec < 1.5, "Process terminated promptly on timeout (< 1.5 s)");
}

/**
 * @brief Validates interprocess pipe redirection and stdout buffer capture.
 */
static void test_output_capture(void) {
    printf("[TEST SUITE] Output Capture\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.capture_output = true;
    opts.command = "bin\\cpu_burn.exe 50";

    crr_metrics_t m;
    crr_output_t out;
    crr_execute(&opts, &m, &out);

    TEST_ASSERT(out.stdout_buf != NULL, "Captured stdout buffer is non-null");
    TEST_ASSERT(strstr(out.stdout_buf, "cpu_burn completed") != NULL, "Stdout captured expected message");
    crr_free_output(&out);
}

/**
 * @brief Validates Windows Job Object process tree tracking across parent and spawned child tasks.
 */
static void test_child_process_tree_tracking(void) {
    printf("[TEST SUITE] Process Tree & Job Object Tracking\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.quiet = true;
    opts.track_children = true;
    opts.command = "bin\\child_spawner.exe";

    crr_metrics_t m;
    crr_execute(&opts, &m, NULL);

    TEST_ASSERT(m.child_processes_tracked == true, "Job Object child tracking active");
    TEST_ASSERT(m.process_count >= 2, "Multiple processes tracked in process tree");
    TEST_ASSERT(m.max_rss_bytes >= (15ULL * 1024ULL * 1024ULL), "Child memory accounted in aggregate peak");
    /* The 25 MiB child faults thousands of pages; a root-only reading stays near 1.2k. */
    TEST_ASSERT(m.page_faults >= 5000, "Page faults aggregated across the process tree");
    /* Each workload writes one status line; the root alone accounts for two operations. */
    TEST_ASSERT(m.io_write_ops >= 3, "I/O writes aggregated across the process tree");
}

/**
 * @brief Verifies that every backslash inside the serialized document introduces
 * a legal JSON escape sequence. Windows command lines contain raw backslashes, so
 * an unescaped string makes the whole document unparsable.
 */
static bool json_escapes_valid(const char *json) {
    for (const char *p = json; *p; ++p) {
        if (*p != '\\') continue;
        ++p;
        switch (*p) {
            case '"': case '\\': case '/': case 'b':
            case 'f': case 'n': case 'r': case 't':
                break;
            case 'u':
                for (int i = 0; i < 4; ++i) {
                    ++p;
                    if (!isxdigit((unsigned char)*p)) return false;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

/**
 * @brief Validates JSON and CSV escaping of command strings containing Windows
 * path separators, quotes, and control characters.
 */
static void test_serializer_escaping(void) {
    printf("[TEST SUITE] Serializer Escaping\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.command = "C:\\tools\\my \"app\".exe";
    opts.working_dir = "C:\\work\\dir";

    crr_metrics_t m;
    memset(&m, 0, sizeof(m));
    m.wall_time_sec = 0.5;

    char json_buf[4096];
    int jres = crr_format_json(&m, &opts, json_buf, sizeof(json_buf));
    TEST_ASSERT(jres == 0, "JSON with backslashes and quotes formats successfully");
    TEST_ASSERT(strstr(json_buf, "\"command\": \"C:\\\\tools\\\\my \\\"app\\\".exe\"") != NULL,
                "JSON command field escapes backslashes and quotes");
    TEST_ASSERT(strstr(json_buf, "\"working_dir\": \"C:\\\\work\\\\dir\"") != NULL,
                "JSON working_dir field escapes backslashes");
    TEST_ASSERT(json_escapes_valid(json_buf), "JSON document contains only legal escape sequences");

    char csv_buf[4096];
    int cres = crr_format_csv(&m, &opts, csv_buf, sizeof(csv_buf), true);
    TEST_ASSERT(cres == 0, "CSV with quotes formats successfully");
    TEST_ASSERT(strstr(csv_buf, "\"C:\\tools\\my \"\"app\"\".exe\"") != NULL,
                "CSV command field doubles embedded quotes");

    crr_stats_summary_t stats;
    memset(&stats, 0, sizeof(stats));
    stats.runs_count = 1;
    int sres = crr_format_stats_json(&stats, &m, &opts, json_buf, sizeof(json_buf));
    TEST_ASSERT(sres == 0, "Benchmark JSON formats successfully");
    TEST_ASSERT(json_escapes_valid(json_buf), "Benchmark JSON contains only legal escape sequences");
}

/**
 * @brief Validates argument checks, buffer-capacity reporting, and unwritable
 * output paths. The CLI checks are Windows-specific because the test suite
 * launches the Windows binaries.
 */
static void test_error_handling(void) {
    printf("[TEST SUITE] Error Handling & Validation\n");

    crr_metrics_t m;
    TEST_ASSERT(crr_execute(NULL, &m, NULL) == CRR_ERR_INVALID_ARGS,
                "NULL options are rejected");
    TEST_ASSERT(crr_execute(&(crr_options_t){ .command = "" }, &m, NULL) == CRR_ERR_INVALID_ARGS,
                "Empty command is rejected");

    char small[8];
    crr_options_t opts;
    crr_options_init(&opts);
    opts.command = "cmd";
    TEST_ASSERT(crr_format_json(&m, &opts, small, sizeof(small)) == -1,
                "Formatter reports insufficient buffer capacity");

    TEST_ASSERT(crr_write_output("report", "Z:\\crr_no_such_dir\\report.txt") == -1,
                "Unwritable output path returns an error");

#if defined(_WIN32)
    int bad_timeout = system("bin\\crr.exe -t nonsense -- bin\\cpu_burn.exe 10 > NUL 2>&1");
    TEST_ASSERT(bad_timeout != 0, "CLI rejects a malformed timeout value");

    int bad_output = system("bin\\crr.exe -o \"Z:\\crr_no_such_dir\\report.txt\" -- bin\\cpu_burn.exe 10 > NUL 2>&1");
    TEST_ASSERT(bad_output != 0, "CLI reports a nonzero status when the report cannot be written");
#endif
}

/**
 * @brief Reproduces the benchmark-mode capture deadlock: capture enabled with no
 * output sink must still drain the pipes, otherwise a verbose child blocks until
 * the timeout fires.
 */
static void test_capture_without_sink(void) {
    printf("[TEST SUITE] Pipe Drain Without Output Sink\n");
    crr_options_t opts;
    crr_options_init(&opts);
    opts.capture_output = true;
    opts.quiet = false;
    opts.timeout_ms = 5000;
    opts.command = "bin\\output_flood.exe 200000";

    crr_metrics_t m;
    int res = crr_execute(&opts, &m, NULL);

    TEST_ASSERT(!m.timed_out, "Flooding child is not killed by the timeout watchdog");
    TEST_ASSERT(res == CRR_SUCCESS && m.exit_code == 0, "Flooding child completes successfully");
    TEST_ASSERT(m.wall_time_sec < 3.0, "Flooding child completes well before the 5 s timeout");
}

/**
 * @brief A command that cannot be resolved must surface as a spawn failure.
 *
 * Regression: the executor used to hand an unresolvable command to cmd.exe /c,
 * which launched fine and then exited 1, so the report claimed SUCCESS for a
 * program that never existed.
 */
static void test_missing_command_classification(void) {
    printf("[TEST SUITE] Missing Command Classification\n");
    crr_options_t opts;
    crr_metrics_t m;

    crr_options_init(&opts);
    opts.quiet = true;
    opts.timeout_ms = 5000;
    opts.command = "crr_missing_command_xyz";
    int res = crr_execute(&opts, &m, NULL);
    TEST_ASSERT(res == CRR_ERR_SPAWN_FAILED, "Unknown bare command reports CRR_ERR_SPAWN_FAILED");
    TEST_ASSERT(m.status_code == CRR_ERR_SPAWN_FAILED, "Metrics record the launch failure");

    crr_options_init(&opts);
    opts.quiet = true;
    opts.timeout_ms = 5000;
    opts.command = "C:\\no_such_dir_xyz\\app.exe";
    res = crr_execute(&opts, &m, NULL);
    TEST_ASSERT(res == CRR_ERR_SPAWN_FAILED, "Nonexistent absolute path reports CRR_ERR_SPAWN_FAILED");

    /* Legitimate non-zero exits and shell built-ins must keep working. */
    crr_options_init(&opts);
    opts.quiet = true;
    opts.timeout_ms = 5000;
    opts.command = "bin\\fail_crash.exe 42";
    res = crr_execute(&opts, &m, NULL);
    TEST_ASSERT(res == CRR_SUCCESS && m.exit_code == 42,
                "Real command exiting 42 is still a successful execution");

    crr_options_init(&opts);
    opts.quiet = true;
    opts.timeout_ms = 5000;
    opts.command = "echo crr_builtin_probe";
    res = crr_execute(&opts, &m, NULL);
    TEST_ASSERT(res == CRR_SUCCESS && m.exit_code == 0,
                "cmd.exe built-in still runs through the shell fallback");

    /* A quoted first token is a program path, not a shell command line. */
    crr_options_init(&opts);
    opts.quiet = true;
    opts.timeout_ms = 5000;
    opts.command = "\"echo hi\"";
    res = crr_execute(&opts, &m, NULL);
    TEST_ASSERT(res == CRR_ERR_SPAWN_FAILED,
                "Quoted non-existent program is not mistaken for a shell line");
}

/**
 * @brief Main test runner entry point.
 */
int main(void) {
    printf("====================================================================\n");
    printf(" COMMAND RESOURCE REPORTER (CRR) - COMPREHENSIVE TEST RUNNER\n");
    printf("====================================================================\n\n");

    /* Execute Unit Tests */
    test_options_init();
    test_byte_and_time_formatters();
    test_stats_math();
    test_json_and_csv_serialization();
    test_serializer_escaping();
    test_error_handling();

    /* Execute Systems / Integration Tests */
    test_exit_codes();
    test_wall_clock_measurement();
    test_cpu_time_measurement();
    test_max_rss_measurement();
    test_timeout_enforcement();
    test_output_capture();
    test_capture_without_sink();
    test_missing_command_classification();
    test_child_process_tree_tracking();

    printf("\n====================================================================\n");
    printf(" TEST SUMMARY: Total: %d | Passed: %d | Failed: %d\n", total_tests, passed_tests, failed_tests);
    printf("====================================================================\n");

    return (failed_tests == 0) ? 0 : 1;
}
