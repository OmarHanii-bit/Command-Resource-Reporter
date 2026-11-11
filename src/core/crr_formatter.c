/**
 * @file crr_formatter.c
 * @brief Implementation of report formatters, unit converters, and serializers.
 *
 * This module converts internal metrics structures into:
 * 1. Human-Readable ASCII reports (with aligned columns and binary unit prefixes).
 * 2. RFC 8259-compliant JSON trees (suitable for automated data ingestion).
 * 3. RFC 4180-compliant CSV tabular streams (for Excel, R, or Python pandas analysis).
 */

#include "crr_formatter.h"
#include "crr_version.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

/**
 * @brief Duplicates a string with JSON string-body escaping applied.
 *
 * Escapes quotes, backslashes, and control characters. Windows command lines
 * contain backslashes (e.g. "bin\app.exe"), which are illegal JSON escapes if
 * emitted verbatim. Returns a heap string the caller must free, or NULL on
 * allocation failure.
 *
 * @param src Source string (NULL is treated as empty)
 * @return Newly allocated escaped string, or NULL
 */
static char* json_escape_dup(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);
    if (len > (SIZE_MAX - 1) / 6) return NULL; /* \u00XX worst case */

    char *out = (char*)malloc(len * 6 + 1);
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)src[i];
        switch (c) {
            case '"':  out[o++] = '\\'; out[o++] = '"';  break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\b': out[o++] = '\\'; out[o++] = 'b';  break;
            case '\f': out[o++] = '\\'; out[o++] = 'f';  break;
            case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
            case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
            case '\t': out[o++] = '\\'; out[o++] = 't';  break;
            default:
                if (c < 0x20) {
                    o += (size_t)snprintf(out + o, len * 6 + 1 - o, "\\u%04x", c);
                } else {
                    out[o++] = (char)c;
                }
                break;
        }
    }
    out[o] = '\0';
    return out;
}

/**
 * @brief Duplicates a string with RFC 4180 field escaping applied (quotes doubled).
 *
 * The caller adds the surrounding quotes. Returns a heap string to free, or NULL.
 *
 * @param src Source string (NULL is treated as empty)
 * @return Newly allocated escaped string, or NULL
 */
static char* csv_escape_dup(const char *src) {
    if (!src) src = "";
    size_t len = strlen(src);
    size_t quotes = 0;
    for (size_t i = 0; i < len; ++i) {
        if (src[i] == '"') quotes++;
    }
    if (len > SIZE_MAX - quotes - 1) return NULL;

    char *out = (char*)malloc(len + quotes + 1);
    if (!out) return NULL;

    size_t o = 0;
    for (size_t i = 0; i < len; ++i) {
        if (src[i] == '"') out[o++] = '"';
        out[o++] = src[i];
    }
    out[o] = '\0';
    return out;
}

/**
 * @brief Formats a byte quantity into binary-multiple units (B, KiB, MiB, GiB, TiB).
 *
 * Uses the IEC binary standard (1 KiB = 1024 B).
 *
 * @param bytes    Raw byte count
 * @param buf      Output character buffer
 * @param buf_len  Capacity of output buffer
 * @return Pointer to buf
 */
const char* crr_format_bytes(uint64_t bytes, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return "";
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit_idx = 0;
    double count = (double)bytes;

    while (count >= 1024.0 && unit_idx < 4) {
        count /= 1024.0;
        unit_idx++;
    }

    if (unit_idx == 0) {
        snprintf(buf, buf_len, "%" PRIu64 " B", bytes);
    } else {
        snprintf(buf, buf_len, "%.2f %s", count, units[unit_idx]);
    }
    return buf;
}

/**
 * @brief Formats seconds into human-readable representation.
 *
 * Chooses appropriate precision based on magnitude:
 * - Sub-second times: formatted in milliseconds ("%.2f ms")
 * - Under 1 minute: formatted in seconds ("%.4f s")
 * - Over 1 minute: formatted in minutes and seconds ("%dm %.2fs")
 *
 * @param seconds  Floating point time in seconds
 * @param buf      Output buffer
 * @param buf_len  Buffer capacity
 * @return Pointer to buf
 */
const char* crr_format_time(double seconds, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return "";

    if (seconds < 0.0) {
        snprintf(buf, buf_len, "0.00 ms");
    } else if (seconds < 1.0) {
        snprintf(buf, buf_len, "%.2f ms", seconds * 1000.0);
    } else if (seconds < 60.0) {
        snprintf(buf, buf_len, "%.4f s", seconds);
    } else {
        int minutes = (int)(seconds / 60.0);
        double rem_sec = seconds - (minutes * 60.0);
        snprintf(buf, buf_len, "%dm %.2fs", minutes, rem_sec);
    }
    return buf;
}

/**
 * @brief Formats a single run into a human-readable ASCII summary table.
 */
int crr_format_human(const crr_metrics_t *m, const crr_options_t *opts, char *buf, size_t max_len) {
    if (!m || !opts || !buf || max_len == 0) return -1;

    char wall_buf[64], user_buf[64], kern_buf[64], tot_buf[64];
    char rss_buf[64], pagefile_buf[64], read_buf[64], write_buf[64];

    /* Format individual metrics into temporary display strings */
    crr_format_time(m->wall_time_sec, wall_buf, sizeof(wall_buf));
    crr_format_time(m->user_cpu_time_sec, user_buf, sizeof(user_buf));
    crr_format_time(m->kernel_cpu_time_sec, kern_buf, sizeof(kern_buf));
    crr_format_time(m->total_cpu_time_sec, tot_buf, sizeof(tot_buf));

    crr_format_bytes(m->max_rss_bytes, rss_buf, sizeof(rss_buf));
    crr_format_bytes(m->peak_pagefile_bytes, pagefile_buf, sizeof(pagefile_buf));
    crr_format_bytes(m->io_read_bytes, read_buf, sizeof(read_buf));
    crr_format_bytes(m->io_write_bytes, write_buf, sizeof(write_buf));

    const char *status_str = (m->status_code == CRR_SUCCESS) ? "SUCCESS" :
                             (m->timed_out ? "TIMED OUT" : "FAILED");

    /* A failed launch has no child exit code to report; name the real cause instead. */
    char status_line[96];
    if (m->status_code == CRR_ERR_SPAWN_FAILED) {
        snprintf(status_line, sizeof(status_line), "FAILED (Launch Error: command not found)");
    } else {
        snprintf(status_line, sizeof(status_line), "%s (Exit Code: %d)", status_str, m->exit_code);
    }

    int written = snprintf(buf, max_len,
        "================================================================================\n"
        " COMMAND RESOURCE USAGE REPORT (%s)\n"
        "================================================================================\n"
        " Command Line         : %s\n"
        " Working Directory    : %s\n"
        " Status               : %s\n"
        " Child Processes      : %u tracked%s\n"
        "--------------------------------------------------------------------------------\n"
        " TIME METRICS\n"
        "   Wall-Clock Elapsed : %12.6f s  (%s)\n"
        "   User CPU Time      : %12.6f s  (%s)\n"
        "   Kernel/System CPU  : %12.6f s  (%s)\n"
        "   Total CPU Time     : %12.6f s  (%s)\n"
        "   CPU Utilization    : %12.2f %%\n"
        "--------------------------------------------------------------------------------\n"
        " MEMORY METRICS\n"
        "   Max Resident Set   : %12" PRIu64 " B  (%s)\n"
        "   Peak Commit Charge : %12" PRIu64 " B  (%s)\n"
        "   Page Faults        : %12" PRIu64 "\n"
        "--------------------------------------------------------------------------------\n"
        " I/O METRICS\n"
        "   File / Device Read : %12" PRIu64 " B  (%s, %" PRIu64 " ops)\n"
        "   File / Device Write: %12" PRIu64 " B  (%s, %" PRIu64 " ops)\n"
        "================================================================================\n",
        CRR_VERSION_STRING,
        opts->command ? opts->command : "<empty>",
        opts->working_dir ? opts->working_dir : "<current>",
        status_line,
        m->process_count, m->child_processes_tracked ? " [Job Object tree active]" : "",
        m->wall_time_sec, wall_buf,
        m->user_cpu_time_sec, user_buf,
        m->kernel_cpu_time_sec, kern_buf,
        m->total_cpu_time_sec, tot_buf,
        m->cpu_utilization_pct,
        m->max_rss_bytes, rss_buf,
        m->peak_pagefile_bytes, pagefile_buf,
        m->page_faults,
        m->io_read_bytes, read_buf, m->io_read_ops,
        m->io_write_bytes, write_buf, m->io_write_ops
    );

    return (written >= 0 && (size_t)written < max_len) ? 0 : -1;
}

/**
 * @brief Serializes a single execution's metrics into JSON format.
 */
int crr_format_json(const crr_metrics_t *m, const crr_options_t *opts, char *buf, size_t max_len) {
    if (!m || !opts || !buf || max_len == 0) return -1;

    char rss_buf[64], wall_buf[64];
    crr_format_bytes(m->max_rss_bytes, rss_buf, sizeof(rss_buf));
    crr_format_time(m->wall_time_sec, wall_buf, sizeof(wall_buf));

    char *cmd_escaped = json_escape_dup(opts->command ? opts->command : "");
    char *dir_escaped = json_escape_dup(opts->working_dir ? opts->working_dir : "");
    if (!cmd_escaped || !dir_escaped) {
        free(cmd_escaped);
        free(dir_escaped);
        return -1;
    }

    int written = snprintf(buf, max_len,
        "{\n"
        "  \"version\": \"%s\",\n"
        "  \"command\": \"%s\",\n"
        "  \"working_dir\": \"%s\",\n"
        "  \"status\": %d,\n"
        "  \"status_name\": \"%s\",\n"
        "  \"exit_code\": %d,\n"
        "  \"timed_out\": %s,\n"
        "  \"process_count\": %u,\n"
        "  \"child_processes_tracked\": %s,\n"
        "  \"metrics\": {\n"
        "    \"wall_time_sec\": %.6f,\n"
        "    \"wall_time_formatted\": \"%s\",\n"
        "    \"user_cpu_sec\": %.6f,\n"
        "    \"kernel_cpu_sec\": %.6f,\n"
        "    \"total_cpu_sec\": %.6f,\n"
        "    \"cpu_utilization_pct\": %.2f,\n"
        "    \"max_rss_bytes\": %" PRIu64 ",\n"
        "    \"max_rss_formatted\": \"%s\",\n"
        "    \"peak_pagefile_bytes\": %" PRIu64 ",\n"
        "    \"page_faults\": %" PRIu64 ",\n"
        "    \"io_read_bytes\": %" PRIu64 ",\n"
        "    \"io_write_bytes\": %" PRIu64 ",\n"
        "    \"io_read_ops\": %" PRIu64 ",\n"
        "    \"io_write_ops\": %" PRIu64 "\n"
        "  }\n"
        "}\n",
        CRR_VERSION_STRING,
        cmd_escaped,
        dir_escaped,
        m->status_code,
        (m->status_code == CRR_SUCCESS) ? "SUCCESS" : (m->timed_out ? "TIMED_OUT" : "ERROR"),
        m->exit_code,
        m->timed_out ? "true" : "false",
        m->process_count,
        m->child_processes_tracked ? "true" : "false",
        m->wall_time_sec,
        wall_buf,
        m->user_cpu_time_sec,
        m->kernel_cpu_time_sec,
        m->total_cpu_time_sec,
        m->cpu_utilization_pct,
        m->max_rss_bytes,
        rss_buf,
        m->peak_pagefile_bytes,
        m->page_faults,
        m->io_read_bytes,
        m->io_write_bytes,
        m->io_read_ops,
        m->io_write_ops
    );

    free(cmd_escaped);
    free(dir_escaped);

    return (written >= 0 && (size_t)written < max_len) ? 0 : -1;
}

/**
 * @brief Formats a single execution's metrics into CSV format.
 */
int crr_format_csv(const crr_metrics_t *m, const crr_options_t *opts, char *buf, size_t max_len, bool include_header) {
    if (!m || !opts || !buf || max_len == 0) return -1;

    size_t offset = 0;
    if (include_header) {
        int h = snprintf(buf, max_len,
            "command,exit_code,status,timed_out,wall_time_sec,user_cpu_sec,kernel_cpu_sec,"
            "total_cpu_sec,cpu_utilization_pct,max_rss_bytes,page_faults,io_read_bytes,io_write_bytes\n");
        if (h < 0 || (size_t)h >= max_len) return -1;
        offset = (size_t)h;
    }

    char *cmd_escaped = csv_escape_dup(opts->command ? opts->command : "");
    if (!cmd_escaped) return -1;

    int r = snprintf(buf + offset, max_len - offset,
        "\"%s\",%d,%d,%s,%.6f,%.6f,%.6f,%.6f,%.2f,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
        cmd_escaped,
        m->exit_code,
        m->status_code,
        m->timed_out ? "true" : "false",
        m->wall_time_sec,
        m->user_cpu_time_sec,
        m->kernel_cpu_time_sec,
        m->total_cpu_time_sec,
        m->cpu_utilization_pct,
        m->max_rss_bytes,
        m->page_faults,
        m->io_read_bytes,
        m->io_write_bytes
    );

    free(cmd_escaped);

    return (r >= 0 && (offset + (size_t)r) < max_len) ? 0 : -1;
}

/**
 * @brief Formats multi-run benchmark statistics into a comparative table.
 */
int crr_format_stats_human(const crr_stats_summary_t *s, const crr_options_t *opts, char *buf, size_t max_len) {
    if (!s || !opts || !buf || max_len == 0) return -1;

    char rss_mean[64], rss_min[64], rss_max[64];
    crr_format_bytes((uint64_t)s->max_rss_bytes.mean, rss_mean, sizeof(rss_mean));
    crr_format_bytes((uint64_t)s->max_rss_bytes.min, rss_min, sizeof(rss_min));
    crr_format_bytes((uint64_t)s->max_rss_bytes.max, rss_max, sizeof(rss_max));

    int written = snprintf(buf, max_len,
        "================================================================================\n"
        " BENCHMARK STATISTICAL SUMMARY (%zu Runs) - CRR %s\n"
        "================================================================================\n"
        " Command Line      : %s\n"
        " Runs Completed    : %zu (Success: %zu, Failed: %zu, Timed Out: %zu)\n"
        "--------------------------------------------------------------------------------\n"
        " %-20s | %10s | %10s | %10s | %10s\n"
        "--------------------------------------------------------------------------------\n"
        " Wall-Clock Time (s) : %10.4f | %10.4f | %10.4f | %10.4f\n"
        " User CPU Time (s)   : %10.4f | %10.4f | %10.4f | %10.4f\n"
        " Kernel CPU Time (s) : %10.4f | %10.4f | %10.4f | %10.4f\n"
        " Total CPU Time (s)  : %10.4f | %10.4f | %10.4f | %10.4f\n"
        " CPU Utilization (%%) : %10.2f | %10.2f | %10.2f | %10.2f\n"
        " Max RSS (Bytes)     : %10.0f | %10.0f | %10.0f | %10.0f\n"
        "   [Max RSS Format]  : %10s | %10s | %10s | %10s\n"
        " Page Faults         : %10.0f | %10.0f | %10.0f | %10.0f\n"
        " I/O Read (Bytes)    : %10.0f | %10.0f | %10.0f | %10.0f\n"
        " I/O Write (Bytes)   : %10.0f | %10.0f | %10.0f | %10.0f\n"
        "================================================================================\n",
        s->runs_count, CRR_VERSION_STRING,
        opts->command ? opts->command : "<empty>",
        s->runs_count, s->successful_runs, s->failed_runs, s->timed_out_runs,
        "Metric", "Mean", "Min", "Max", "StdDev",
        s->wall_time.mean, s->wall_time.min, s->wall_time.max, s->wall_time.stddev,
        s->user_cpu.mean, s->user_cpu.min, s->user_cpu.max, s->user_cpu.stddev,
        s->kernel_cpu.mean, s->kernel_cpu.min, s->kernel_cpu.max, s->kernel_cpu.stddev,
        s->total_cpu.mean, s->total_cpu.min, s->total_cpu.max, s->total_cpu.stddev,
        s->cpu_utilization.mean, s->cpu_utilization.min, s->cpu_utilization.max, s->cpu_utilization.stddev,
        s->max_rss_bytes.mean, s->max_rss_bytes.min, s->max_rss_bytes.max, s->max_rss_bytes.stddev,
        rss_mean, rss_min, rss_max, "-",
        s->page_faults.mean, s->page_faults.min, s->page_faults.max, s->page_faults.stddev,
        s->io_read_bytes.mean, s->io_read_bytes.min, s->io_read_bytes.max, s->io_read_bytes.stddev,
        s->io_write_bytes.mean, s->io_write_bytes.min, s->io_write_bytes.max, s->io_write_bytes.stddev
    );

    return (written >= 0 && (size_t)written < max_len) ? 0 : -1;
}

/**
 * @brief Serializes benchmark statistics and individual runs into JSON format.
 */
int crr_format_stats_json(const crr_stats_summary_t *s, const crr_metrics_t *runs, const crr_options_t *opts, char *buf, size_t max_len) {
    if (!s || !opts || !buf || max_len == 0) return -1;

    char *cmd_escaped = json_escape_dup(opts->command ? opts->command : "");
    if (!cmd_escaped) return -1;

    size_t offset = 0;
    int w = snprintf(buf, max_len,
        "{\n"
        "  \"version\": \"%s\",\n"
        "  \"command\": \"%s\",\n"
        "  \"runs_count\": %zu,\n"
        "  \"successful_runs\": %zu,\n"
        "  \"failed_runs\": %zu,\n"
        "  \"timed_out_runs\": %zu,\n"
        "  \"summary\": {\n"
        "    \"wall_time\": {\"mean\": %.6f, \"min\": %.6f, \"max\": %.6f, \"stddev\": %.6f},\n"
        "    \"user_cpu\": {\"mean\": %.6f, \"min\": %.6f, \"max\": %.6f, \"stddev\": %.6f},\n"
        "    \"kernel_cpu\": {\"mean\": %.6f, \"min\": %.6f, \"max\": %.6f, \"stddev\": %.6f},\n"
        "    \"total_cpu\": {\"mean\": %.6f, \"min\": %.6f, \"max\": %.6f, \"stddev\": %.6f},\n"
        "    \"cpu_utilization\": {\"mean\": %.2f, \"min\": %.2f, \"max\": %.2f, \"stddev\": %.2f},\n"
        "    \"max_rss_bytes\": {\"mean\": %.0f, \"min\": %.0f, \"max\": %.0f, \"stddev\": %.0f},\n"
        "    \"page_faults\": {\"mean\": %.0f, \"min\": %.0f, \"max\": %.0f, \"stddev\": %.0f},\n"
        "    \"io_read_bytes\": {\"mean\": %.0f, \"min\": %.0f, \"max\": %.0f, \"stddev\": %.0f},\n"
        "    \"io_write_bytes\": {\"mean\": %.0f, \"min\": %.0f, \"max\": %.0f, \"stddev\": %.0f}\n"
        "  },\n"
        "  \"runs\": [\n",
        CRR_VERSION_STRING,
        cmd_escaped,
        s->runs_count, s->successful_runs, s->failed_runs, s->timed_out_runs,
        s->wall_time.mean, s->wall_time.min, s->wall_time.max, s->wall_time.stddev,
        s->user_cpu.mean, s->user_cpu.min, s->user_cpu.max, s->user_cpu.stddev,
        s->kernel_cpu.mean, s->kernel_cpu.min, s->kernel_cpu.max, s->kernel_cpu.stddev,
        s->total_cpu.mean, s->total_cpu.min, s->total_cpu.max, s->total_cpu.stddev,
        s->cpu_utilization.mean, s->cpu_utilization.min, s->cpu_utilization.max, s->cpu_utilization.stddev,
        s->max_rss_bytes.mean, s->max_rss_bytes.min, s->max_rss_bytes.max, s->max_rss_bytes.stddev,
        s->page_faults.mean, s->page_faults.min, s->page_faults.max, s->page_faults.stddev,
        s->io_read_bytes.mean, s->io_read_bytes.min, s->io_read_bytes.max, s->io_read_bytes.stddev,
        s->io_write_bytes.mean, s->io_write_bytes.min, s->io_write_bytes.max, s->io_write_bytes.stddev
    );

    free(cmd_escaped);
    if (w < 0 || (size_t)w >= max_len) return -1;
    offset = (size_t)w;

    if (runs) {
        for (size_t i = 0; i < s->runs_count; ++i) {
            int rw = snprintf(buf + offset, max_len - offset,
                "    {\"run\": %zu, \"exit_code\": %d, \"wall_time_sec\": %.6f, \"total_cpu_sec\": %.6f, \"max_rss_bytes\": %" PRIu64 "}%s\n",
                i + 1, runs[i].exit_code, runs[i].wall_time_sec, runs[i].total_cpu_time_sec, runs[i].max_rss_bytes,
                (i + 1 < s->runs_count) ? "," : "");
            if (rw < 0 || (offset + (size_t)rw) >= max_len) return -1;
            offset += (size_t)rw;
        }
    }

    int end = snprintf(buf + offset, max_len - offset, "  ]\n}\n");
    if (end < 0 || (offset + (size_t)end) >= max_len) return -1;

    return 0;
}

/**
 * @brief Formats benchmark statistics and individual runs into CSV format.
 */
int crr_format_stats_csv(const crr_stats_summary_t *s, const crr_metrics_t *runs, const crr_options_t *opts, char *buf, size_t max_len) {
    if (!s || !opts || !buf || max_len == 0) return -1;

    char *cmd_escaped = csv_escape_dup(opts->command ? opts->command : "");
    if (!cmd_escaped) return -1;

    size_t offset = 0;
    int h = snprintf(buf, max_len,
        "run,command,exit_code,wall_time_sec,user_cpu_sec,kernel_cpu_sec,total_cpu_sec,cpu_utilization_pct,max_rss_bytes\n");
    if (h < 0 || (size_t)h >= max_len) {
        free(cmd_escaped);
        return -1;
    }
    offset = (size_t)h;

    if (runs) {
        for (size_t i = 0; i < s->runs_count; ++i) {
            int r = snprintf(buf + offset, max_len - offset,
                "%zu,\"%s\",%d,%.6f,%.6f,%.6f,%.6f,%.2f,%" PRIu64 "\n",
                i + 1,
                cmd_escaped,
                runs[i].exit_code,
                runs[i].wall_time_sec,
                runs[i].user_cpu_time_sec,
                runs[i].kernel_cpu_time_sec,
                runs[i].total_cpu_time_sec,
                runs[i].cpu_utilization_pct,
                runs[i].max_rss_bytes
            );
            if (r < 0 || (offset + (size_t)r) >= max_len) {
                free(cmd_escaped);
                return -1;
            }
            offset += (size_t)r;
        }
    }

    /* Append summary statistical rows at the end of the CSV */
    int sm = snprintf(buf + offset, max_len - offset,
        "\"MEAN\",\"%s\",0,%.6f,%.6f,%.6f,%.6f,%.2f,%.0f\n"
        "\"STDDEV\",\"%s\",0,%.6f,%.6f,%.6f,%.6f,%.2f,%.0f\n",
        cmd_escaped,
        s->wall_time.mean, s->user_cpu.mean, s->kernel_cpu.mean, s->total_cpu.mean, s->cpu_utilization.mean, s->max_rss_bytes.mean,
        cmd_escaped,
        s->wall_time.stddev, s->user_cpu.stddev, s->kernel_cpu.stddev, s->total_cpu.stddev, s->cpu_utilization.stddev, s->max_rss_bytes.stddev
    );
    free(cmd_escaped);
    if (sm < 0 || (offset + (size_t)sm) >= max_len) return -1;

    return 0;
}

/**
 * @brief Routes formatted string output to disk file or standard output.
 */
int crr_write_output(const char *text, const char *file_path) {
    if (!text) return -1;

    /* If no file path or "-" is specified, write directly to standard output */
    if (!file_path || strcmp(file_path, "-") == 0 || strlen(file_path) == 0) {
        fputs(text, stdout);
        fflush(stdout);
        return 0;
    }

    FILE *f = fopen(file_path, "w");
    if (!f) return -1;

    size_t len = strlen(text);
    size_t written = fwrite(text, 1, len, f);
    fclose(f);

    return (written == len) ? 0 : -1;
}
