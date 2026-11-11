/**
 * @file crr_stats.c
 * @brief Statistical calculation algorithms for multi-run benchmarks.
 *
 * This module computes descriptive statistics for multi-run evaluations.
 *
 * Theory and Numerical Stability:
 * When computing sample variance, standard one-pass algorithms (\sum x^2 - (\sum x)^2 / N)
 * suffer from catastrophic cancellation when the variance is small compared to the squared mean.
 * This implementation uses a numerically stable two-pass algorithm:
 *   Pass 1: Computes the arithmetic sample mean (\mu).
 *   Pass 2: Computes squared deviations from the mean ((x_i - \mu)^2).
 *
 * Bessel's Correction:
 * Sample variance uses denominator (N - 1) instead of N to yield an unbiased estimator
 * of the population variance for small sample sizes:
 *   s^2 = \frac{1}{N - 1} \sum_{i=1}^{N} (x_i - \bar{x})^2
 */

#include "crr_stats.h"
#include <stdlib.h>
#include <math.h>
#include <string.h>

/**
 * @brief Computes min, max, mean, and standard deviation for a continuous double array.
 *
 * @param values  Array of double-precision floating point observations
 * @param count   Number of elements in the array
 * @param out     Output pointer for the statistical metric container
 */
static void compute_single_metric(const double *values, size_t count, crr_stat_metric_t *out) {
    if (count == 0) {
        memset(out, 0, sizeof(*out));
        return;
    }

    /* Pass 1: Find min, max, and compute sum for arithmetic mean */
    double min = values[0];
    double max = values[0];
    double sum = 0.0;

    for (size_t i = 0; i < count; ++i) {
        double v = values[i];
        if (v < min) min = v;
        if (v > max) max = v;
        sum += v;
    }

    double mean = sum / (double)count;

    /* Pass 2: Compute sum of squared deviations from the sample mean */
    double var_sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        double diff = values[i] - mean;
        var_sum += diff * diff;
    }

    /* Apply Bessel's correction (N - 1) for sample standard deviation */
    double stddev = (count > 1) ? sqrt(var_sum / (double)(count - 1)) : 0.0;

    out->min = min;
    out->max = max;
    out->mean = mean;
    out->stddev = stddev;
}

/**
 * @brief Computes comprehensive multi-run statistics across all resource metrics.
 *
 * Extracts each metric into temporary contiguous arrays and computes individual
 * statistical summaries, while aggregating run success, failure, and timeout tallies.
 *
 * @param runs     Array of individual run metrics
 * @param count    Number of runs in the array
 * @param summary  Output statistical summary structure
 */
void crr_compute_stats(const crr_metrics_t *runs, size_t count, crr_stats_summary_t *summary) {
    if (!runs || count == 0 || !summary) return;

    memset(summary, 0, sizeof(*summary));
    summary->runs_count = count;

    /* Allocate contiguous working arrays for each metric dimension */
    double *wall_times   = (double*)malloc(count * sizeof(double));
    double *user_cpus    = (double*)malloc(count * sizeof(double));
    double *kernel_cpus  = (double*)malloc(count * sizeof(double));
    double *total_cpus   = (double*)malloc(count * sizeof(double));
    double *cpu_utils    = (double*)malloc(count * sizeof(double));
    double *max_rsss     = (double*)malloc(count * sizeof(double));
    double *page_faults  = (double*)malloc(count * sizeof(double));
    double *io_reads     = (double*)malloc(count * sizeof(double));
    double *io_writes    = (double*)malloc(count * sizeof(double));

    /* Verify memory allocations succeeded */
    if (!wall_times || !user_cpus || !kernel_cpus || !total_cpus ||
        !cpu_utils || !max_rsss || !page_faults || !io_reads || !io_writes) {
        free(wall_times); free(user_cpus); free(kernel_cpus); free(total_cpus);
        free(cpu_utils); free(max_rsss); free(page_faults); free(io_reads); free(io_writes);
        return;
    }

    /* Populate metric arrays and count run outcomes */
    for (size_t i = 0; i < count; ++i) {
        const crr_metrics_t *m = &runs[i];

        if (m->timed_out) {
            summary->timed_out_runs++;
        }
        if (m->status_code == CRR_SUCCESS && m->exit_code == 0) {
            summary->successful_runs++;
        } else {
            summary->failed_runs++;
        }

        wall_times[i]   = m->wall_time_sec;
        user_cpus[i]    = m->user_cpu_time_sec;
        kernel_cpus[i]  = m->kernel_cpu_time_sec;
        total_cpus[i]   = m->total_cpu_time_sec;
        cpu_utils[i]    = m->cpu_utilization_pct;
        max_rsss[i]     = (double)m->max_rss_bytes;
        page_faults[i]  = (double)m->page_faults;
        io_reads[i]     = (double)m->io_read_bytes;
        io_writes[i]    = (double)m->io_write_bytes;
    }

    /* Compute statistics for each individual metric */
    compute_single_metric(wall_times, count, &summary->wall_time);
    compute_single_metric(user_cpus, count, &summary->user_cpu);
    compute_single_metric(kernel_cpus, count, &summary->kernel_cpu);
    compute_single_metric(total_cpus, count, &summary->total_cpu);
    compute_single_metric(cpu_utils, count, &summary->cpu_utilization);
    compute_single_metric(max_rsss, count, &summary->max_rss_bytes);
    compute_single_metric(page_faults, count, &summary->page_faults);
    compute_single_metric(io_reads, count, &summary->io_read_bytes);
    compute_single_metric(io_writes, count, &summary->io_write_bytes);

    /* Free temporary working arrays */
    free(wall_times);
    free(user_cpus);
    free(kernel_cpus);
    free(total_cpus);
    free(cpu_utils);
    free(max_rsss);
    free(page_faults);
    free(io_reads);
    free(io_writes);
}
