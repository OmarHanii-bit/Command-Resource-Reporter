/**
 * @file crr_formatter.h
 * @brief Output formatting, serialization, and report generation interface.
 *
 * This header defines functions for converting raw binary metrics and statistical
 * summaries into human-readable tables, RFC 8259-compliant JSON objects, and
 * RFC 4180-compliant CSV rows, as well as writing the formatted outputs to files or stdout.
 */

#ifndef CRR_FORMATTER_H
#define CRR_FORMATTER_H

#include "crr.h"
#include "crr_stats.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Formats a raw byte count into human-readable binary-prefix units (B, KiB, MiB, GiB, TiB).
 *
 * Automatically chooses the most appropriate unit based on binary 1024 thresholds.
 *
 * @param bytes    Raw byte count to format
 * @param buf      Output character buffer
 * @param buf_len  Size of the output buffer in bytes
 * @return Pointer to buf for convenient inline string interpolation
 */
const char* crr_format_bytes(uint64_t bytes, char *buf, size_t buf_len);

/**
 * @brief Formats elapsed seconds into an appropriate human-readable time representation.
 *
 * - Less than 1 second: formats as milliseconds (e.g., "12.34 ms")
 * - 1 to 60 seconds: formats as seconds with 4 decimals (e.g., "4.5678 s")
 * - Greater than 60 seconds: formats as minutes and seconds (e.g., "2m 14.50s")
 *
 * @param seconds  Time value in seconds
 * @param buf      Output character buffer
 * @param buf_len  Size of the output buffer in bytes
 * @return Pointer to buf for inline string interpolation
 */
const char* crr_format_time(double seconds, char *buf, size_t buf_len);

/**
 * @brief Formats a single execution's metrics into an aligned ASCII summary report.
 *
 * @param m        Pointer to measured resource metrics
 * @param opts     Pointer to execution configuration options
 * @param buf      Output character buffer
 * @param max_len  Maximum capacity of the output buffer
 * @return 0 on success, or -1 if the buffer capacity was insufficient
 */
int crr_format_human(const crr_metrics_t *m, const crr_options_t *opts, char *buf, size_t max_len);

/**
 * @brief Serializes a single execution's metrics into RFC 8259-compliant JSON.
 *
 * Includes both raw numerical quantities and formatted string representations.
 *
 * @param m        Pointer to measured resource metrics
 * @param opts     Pointer to execution options
 * @param buf      Output character buffer
 * @param max_len  Maximum capacity of the output buffer
 * @return 0 on success, or -1 if the buffer was too small
 */
int crr_format_json(const crr_metrics_t *m, const crr_options_t *opts, char *buf, size_t max_len);

/**
 * @brief Formats a single execution's metrics into a CSV row.
 *
 * @param m               Pointer to measured resource metrics
 * @param opts            Pointer to execution options
 * @param buf             Output character buffer
 * @param max_len         Maximum capacity of the output buffer
 * @param include_header  Whether to prepend the CSV column header row
 * @return 0 on success, or -1 on buffer overflow
 */
int crr_format_csv(const crr_metrics_t *m, const crr_options_t *opts, char *buf, size_t max_len, bool include_header);

/**
 * @brief Formats multi-run benchmark statistics into a comparative ASCII table.
 *
 * Displays Mean, Min, Max, and Standard Deviation for all measured metrics.
 *
 * @param s        Pointer to computed statistics summary
 * @param opts     Pointer to execution options
 * @param buf      Output character buffer
 * @param max_len  Maximum capacity of the output buffer
 * @return 0 on success, or -1 on buffer overflow
 */
int crr_format_stats_human(const crr_stats_summary_t *s, const crr_options_t *opts, char *buf, size_t max_len);

/**
 * @brief Serializes benchmark statistics and individual run entries into a JSON tree.
 *
 * @param s        Pointer to computed statistics summary
 * @param runs     Pointer to array of individual execution metrics
 * @param opts     Pointer to execution options
 * @param buf      Output character buffer
 * @param max_len  Maximum capacity of the output buffer
 * @return 0 on success, or -1 on buffer overflow
 */
int crr_format_stats_json(const crr_stats_summary_t *s, const crr_metrics_t *runs, const crr_options_t *opts, char *buf, size_t max_len);

/**
 * @brief Formats benchmark statistics and individual runs into a CSV document with summary rows.
 *
 * @param s        Pointer to computed statistics summary
 * @param runs     Pointer to array of individual execution metrics
 * @param opts     Pointer to execution options
 * @param buf      Output character buffer
 * @param max_len  Maximum capacity of the output buffer
 * @return 0 on success, or -1 on buffer overflow
 */
int crr_format_stats_csv(const crr_stats_summary_t *s, const crr_metrics_t *runs, const crr_options_t *opts, char *buf, size_t max_len);

/**
 * @brief Routes formatted text output to a specified file or to standard output.
 *
 * If file_path is NULL, empty, or "-", output is flushed to stdout.
 *
 * @param text       Null-terminated string content to write
 * @param file_path  Destination file path or NULL for stdout
 * @return 0 on success, or -1 on file write/access error
 */
int crr_write_output(const char *text, const char *file_path);

#ifdef __cplusplus
}
#endif

#endif /* CRR_FORMATTER_H */
