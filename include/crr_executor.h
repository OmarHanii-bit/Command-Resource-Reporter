/**
 * @file crr_executor.h
 * @brief Process execution supervisor engine interface.
 *
 * This header defines the public interface for executing target commands,
 * managing the process lifecycle, attaching operating-system monitoring facilities,
 * streaming periodic progress updates, enforcing timeouts, and retrieving resource metrics.
 */

#ifndef CRR_EXECUTOR_H
#define CRR_EXECUTOR_H

#include "crr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Progress callback invoked periodically during active process execution.
 *
 * Used by graphical user interfaces or monitoring daemons to provide real-time
 * telemetry without interrupting process execution.
 *
 * @param user_data          Opaque pointer supplied by the caller
 * @param elapsed_sec        Current elapsed wall-clock time in seconds
 * @param current_rss_bytes  Current resident set size (Working Set) in bytes
 */
typedef void (*crr_progress_cb)(void *user_data, double elapsed_sec, uint64_t current_rss_bytes);

/**
 * @brief Cancellation check callback invoked during execution polling.
 *
 * The supervisor calls this function on every wait loop iteration (every ~20-25 ms).
 * If the callback returns true, the supervisor terminates the process tree immediately.
 *
 * @param user_data Opaque pointer supplied by the caller
 * @return true if execution must be aborted, false to continue execution
 */
typedef bool (*crr_cancel_check_fn)(void *user_data);

/**
 * @brief Executes a command with standard configuration and measures its resource usage.
 *
 * This is the primary synchronous execution entry point. It creates the child process,
 * monitors its progress, awaits its termination, and populates the metrics structure.
 *
 * @param opts     Pointer to populated execution options (must specify opts->command)
 * @param metrics  Output pointer where final resource measurements will be stored
 * @param output   Optional output pointer to receive captured stdout/stderr (can be NULL)
 * @return         CRR_SUCCESS (0) on successful measurement, or an error code
 */
int crr_execute(const crr_options_t *opts, crr_metrics_t *metrics, crr_output_t *output);

/**
 * @brief Executes a command with live progress callbacks and asynchronous cancellation support.
 *
 * Ideal for multi-threaded environments and GUI applications. The supervisor invokes
 * progress_cb at regular intervals with current elapsed time and working set size, and
 * queries cancel_fn to determine if the user requested early termination.
 *
 * @param opts         Pointer to execution options
 * @param metrics      Output pointer for final resource measurements
 * @param output       Output pointer for captured process output (can be NULL)
 * @param progress_cb  Optional callback for live progress updates (can be NULL)
 * @param cancel_fn    Optional callback to check for user cancellation (can be NULL)
 * @param user_data    Opaque caller pointer passed unchanged to callbacks
 * @return             CRR_SUCCESS (0) on success, CRR_ERR_CANCELLED, or other error code
 */
int crr_execute_with_callbacks(const crr_options_t *opts,
                               crr_metrics_t *metrics,
                               crr_output_t *output,
                               crr_progress_cb progress_cb,
                               crr_cancel_check_fn cancel_fn,
                               void *user_data);

/**
 * @brief Releases heap memory buffers allocated inside a crr_output_t structure.
 *
 * Safe to call with NULL pointers or unpopulated structures.
 *
 * @param output Pointer to crr_output_t structure whose buffers should be freed
 */
void crr_free_output(crr_output_t *output);

/**
 * @brief Converts an internal status error code to a user-friendly descriptive string.
 *
 * @param status The status code to convert
 * @return Constant string description (e.g. "Execution timed out")
 */
const char* crr_status_str(crr_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* CRR_EXECUTOR_H */
