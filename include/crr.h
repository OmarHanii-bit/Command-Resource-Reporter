/**
 * @file crr.h
 * @brief Core data structures, enumeration types, and public API definitions.
 *
 * This header defines the primary data transfer structures used throughout CRR:
 * - Execution status codes (crr_status_t)
 * - Serialization format identifiers (crr_format_t)
 * - Process resource usage metrics container (crr_metrics_t)
 * - Execution configuration parameters (crr_options_t)
 * - Interprocess captured output buffers (crr_output_t)
 */

#ifndef CRR_H
#define CRR_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Execution Status and Error Codes.
 *
 * Distinct values representing the outcome of process monitoring.
 */
typedef enum {
    CRR_SUCCESS            = 0,  /**< Execution completed normally without supervisor errors */
    CRR_ERR_INVALID_ARGS   = 1,  /**< Invalid parameters provided (e.g. NULL or empty command) */
    CRR_ERR_SPAWN_FAILED   = 2,  /**< Operating system failed to spawn the target executable */
    CRR_ERR_TIMED_OUT      = 3,  /**< Command exceeded user-specified execution timeout limit */
    CRR_ERR_WAIT_FAILED    = 4,  /**< Failed waiting on process termination status from OS */
    CRR_ERR_METRICS_FAILED = 5,  /**< Querying kernel timers or memory counters failed */
    CRR_ERR_IO             = 6,  /**< Pipe creation, file redirection, or disk write error */
    CRR_ERR_MEMORY         = 7,  /**< Dynamic memory allocation (malloc/realloc) failed */
    CRR_ERR_CANCELLED      = 8   /**< Execution was deliberately aborted by the user (e.g. GUI Cancel) */
} crr_status_t;

/**
 * @brief Output Serialization Format.
 *
 * Determines how the final metrics report is formatted for user display or export.
 */
typedef enum {
    CRR_FORMAT_HUMAN = 0,  /**< Formatted tabular ASCII text for human console inspection */
    CRR_FORMAT_JSON  = 1,  /**< RFC 8259-compliant structured JSON for scripts and machine parsing */
    CRR_FORMAT_CSV   = 2   /**< RFC 4180-compliant comma-separated values for spreadsheet analysis */
} crr_format_t;

/**
 * @brief Process Resource Usage Metrics.
 *
 * Contains empirical resource measurements collected by the supervisor upon target termination.
 */
typedef struct {
    crr_status_t status_code;          /**< Final execution status code */
    int          exit_code;            /**< Exit code returned by target process (0 = success) */
    bool         timed_out;            /**< True if the process was terminated due to timeout */
    bool         terminated_by_signal; /**< True if process was killed by a POSIX signal */
    int          term_signal;          /**< Signal number that terminated process (POSIX only) */

    /* Time Metrics */
    double       wall_time_sec;        /**< Elapsed real/wall-clock time in seconds (sub-microsecond) */
    double       user_cpu_time_sec;    /**< CPU time spent executing in unprivileged user mode */
    double       kernel_cpu_time_sec;  /**< CPU time spent executing inside OS kernel system calls */
    double       total_cpu_time_sec;   /**< Sum of user CPU time and kernel CPU time */
    double       cpu_utilization_pct;  /**< Overall CPU utilization percentage: (total_cpu / wall_time) * 100 */

    /* Memory Metrics */
    uint64_t     max_rss_bytes;        /**< Peak Resident Set Size (Peak Working Set on Windows) in bytes */
    uint64_t     peak_pagefile_bytes;  /**< Peak commit charge / pagefile allocation in bytes */
    uint64_t     page_faults;          /**< Total page faults incurred during execution (soft + hard) */

    /* I/O Metrics */
    uint64_t     io_read_bytes;        /**< Total bytes read from disk/network/devices */
    uint64_t     io_write_bytes;       /**< Total bytes written to disk/network/devices */
    uint64_t     io_read_ops;          /**< Number of distinct read I/O operations */
    uint64_t     io_write_ops;         /**< Number of distinct write I/O operations */

    /* Process Hierarchy Information */
    uint32_t     process_count;        /**< Total processes tracked in the process tree / Job Object */
    bool         child_processes_tracked; /**< True if child process tree aggregation was active */
} crr_metrics_t;

/**
 * @brief Execution Options and Configuration Parameters.
 *
 * Configures the execution environment, timeouts, monitoring features, and output formats.
 */
typedef struct {
    const char  *command;              /**< Target command line string to execute (required) */
    const char  *working_dir;          /**< Initial working directory for child process (NULL = current) */
    uint32_t     timeout_ms;           /**< Timeout limit in milliseconds (0 = no timeout / infinite) */
    uint32_t     repeat_count;         /**< Number of benchmark repetitions (default = 1) */
    bool         track_children;       /**< Track descendant processes via Windows Job Objects */
    bool         capture_output;       /**< Intercept child stdout and stderr into memory buffers */
    bool         verbose;              /**< Enable verbose diagnostic logs */
    bool         quiet;                /**< Suppress child process output completely */
    crr_format_t format;               /**< Selected report output format */
    const char  *output_file;          /**< File path to save report to (NULL = stdout) */
} crr_options_t;

/**
 * @brief Interprocess Output Capture Buffers.
 *
 * Dynamically reallocating memory buffers holding child process standard output and standard error.
 */
typedef struct {
    char   *stdout_buf;                /**< Dynamically allocated buffer containing captured stdout */
    size_t  stdout_len;                /**< Length of captured stdout in bytes (excluding null-terminator) */
    char   *stderr_buf;                /**< Dynamically allocated buffer containing captured stderr */
    size_t  stderr_len;                /**< Length of captured stderr in bytes (excluding null-terminator) */
} crr_output_t;

/**
 * @brief Initializes execution options with safe default values.
 *
 * Sets timeout to 0 (infinite), repeat_count to 1, track_children to true,
 * capture_output to false, quiet to false, and format to CRR_FORMAT_HUMAN.
 *
 * @param opts Pointer to crr_options_t structure to initialize
 */
void crr_options_init(crr_options_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* CRR_H */
