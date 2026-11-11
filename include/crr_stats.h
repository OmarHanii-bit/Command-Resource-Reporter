/**
 * @file crr_stats.h
 * @brief Statistical analysis engine for multi-run benchmark evaluations.
 *
 * This header defines data structures and functions for computing descriptive
 * statistics across multiple executions of the same command. It enables rigorous
 * benchmark comparisons by calculating:
 * - Minimum observed value
 * - Maximum observed value
 * - Arithmetic sample mean: \mu = (1/N) * \sum(x_i)
 * - Corrected sample standard deviation: \sigma = \sqrt{(1/(N-1)) * \sum(x_i - \mu)^2}
 */

#ifndef CRR_STATS_H
#define CRR_STATS_H

#include "crr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Descriptive statistical summary for a single continuous metric.
 */
typedef struct {
    double min;     /**< Minimum observed value across all runs */
    double max;     /**< Maximum observed value across all runs */
    double mean;    /**< Arithmetic sample mean */
    double stddev;  /**< Sample standard deviation (Bessel-corrected with N-1 denominator) */
} crr_stat_metric_t;

/**
 * @brief Aggregated statistical summary covering all resource dimensions.
 */
typedef struct {
    size_t             runs_count;       /**< Total number of benchmark runs evaluated */
    size_t             successful_runs;  /**< Count of runs that exited with status 0 */
    size_t             failed_runs;      /**< Count of runs that returned non-zero exit codes */
    size_t             timed_out_runs;   /**< Count of runs that exceeded the timeout limit */

    crr_stat_metric_t  wall_time;        /**< Elapsed wall-clock time statistics (seconds) */
    crr_stat_metric_t  user_cpu;         /**< User mode CPU time statistics (seconds) */
    crr_stat_metric_t  kernel_cpu;       /**< Kernel mode CPU time statistics (seconds) */
    crr_stat_metric_t  total_cpu;        /**< Total CPU time statistics (seconds) */
    crr_stat_metric_t  cpu_utilization;  /**< CPU utilization percentage statistics (%) */
    crr_stat_metric_t  max_rss_bytes;    /**< Peak Resident Set Size statistics (bytes) */
    crr_stat_metric_t  page_faults;      /**< Page fault count statistics */
    crr_stat_metric_t  io_read_bytes;    /**< File/device read bytes statistics */
    crr_stat_metric_t  io_write_bytes;   /**< File/device write bytes statistics */
} crr_stats_summary_t;

/**
 * @brief Computes statistical metrics across an array of execution measurements.
 *
 * Performs two-pass variance calculation to prevent floating-point catastrophic cancellation.
 * If count is 1, standard deviation is set to 0.0.
 *
 * @param runs     Pointer to array of crr_metrics_t containing individual run results
 * @param count    Number of elements in the runs array
 * @param summary  Output pointer where statistical metrics will be stored
 */
void crr_compute_stats(const crr_metrics_t *runs, size_t count, crr_stats_summary_t *summary);

#ifdef __cplusplus
}
#endif

#endif /* CRR_STATS_H */
