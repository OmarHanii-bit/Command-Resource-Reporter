/**
 * @file main.c
 * @brief Command Line Interface (CLI) entry point and command-line parser.
 *
 * This module implements the console interface for the Command Resource Reporter:
 * 1. Command-Line Argument Parsing:
 *    - Validates syntax and flags (-t, -n, -f, -o, -d, -q, -c, --no-children, -v).
 *    - Recognizes the standard POSIX double-dash ("--") delimiter, treating all
 *      subsequent tokens as part of the target command line without misinterpreting
 *      flags intended for the child command.
 *    - Dynamically reconstructs quoted arguments to preserve spaces and special characters.
 *
 * 2. Execution Modes:
 *    - Single-run mode: Executes the target once and displays immediate resource metrics.
 *    - Benchmark mode (-n <count>): Repeats execution N times, aggregates metrics,
 *      calculates sample mean, min, max, and standard deviation, and emits formatted summaries.
 *
 * 3. Serialization Routing:
 *    - Emits Human-readable tables, JSON, or CSV to stdout or user-specified file (-o).
 */

#include "crr.h"
#include "crr_executor.h"
#include "crr_formatter.h"
#include "crr_stats.h"
#include "crr_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/**
 * @brief Returns the CPU time consumed by the CRR process itself, in 100 ns units.
 *
 * Used to quantify observer overhead: the supervisor's own cost of polling,
 * accounting, and reporting while the child runs.
 */
static uint64_t process_cpu_ticks_100ns(void) {
#ifdef _WIN32
    FILETIME ft_create, ft_exit, ft_kernel, ft_user;
    ULARGE_INTEGER k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &ft_create, &ft_exit, &ft_kernel, &ft_user)) {
        return 0;
    }
    k.LowPart = ft_kernel.dwLowDateTime;
    k.HighPart = ft_kernel.dwHighDateTime;
    u.LowPart = ft_user.dwLowDateTime;
    u.HighPart = ft_user.dwHighDateTime;
    return k.QuadPart + u.QuadPart;
#else
    return (uint64_t)((double)clock() * (10000000.0 / (double)CLOCKS_PER_SEC));
#endif
}

/**
 * @brief Prints program name, version, and copyright banner.
 */
static void print_banner(void) {
    printf("%s v%s\n", CRR_PROGRAM_NAME, CRR_VERSION_STRING);
    printf("%s\n\n", CRR_COPYRIGHT);
}

/**
 * @brief Displays comprehensive help and usage guide.
 */
static void print_help(const char *prog_name) {
    print_banner();
    printf("USAGE:\n");
    printf("  %s [options] -- <command> [args...]\n", prog_name);
    printf("  %s [options] \"<command>\"\n\n", prog_name);
    printf("DESCRIPTION:\n");
    printf("  Executes a command and reports high-precision resource usage including\n");
    printf("  wall-clock time, user/kernel CPU time, CPU utilization %%, peak resident\n");
    printf("  set size (Peak Working Set), page faults, and I/O counters.\n\n");
    printf("OPTIONS:\n");
    printf("  -h, --help               Display this help message and exit\n");
    printf("  -V, --version            Display version information and exit\n");
    printf("  -t, --timeout <ms|s|m>   Set execution timeout (e.g. 500ms, 5s, 2m; default: none)\n");
    printf("  -n, --repeat <count>     Run command N times and report statistical aggregates\n");
    printf("  -f, --format <fmt>       Output format: human (default), json, or csv\n");
    printf("  -o, --output <file>      Write resource report to file instead of stdout\n");
    printf("  -d, --dir <directory>    Set initial working directory for child process\n");
    printf("  -q, --quiet              Suppress child process stdout and stderr\n");
    printf("  -c, --capture            Capture child output into internal buffer\n");
    printf("      --no-children        Disable Windows Job Object child process tree aggregation\n");
    printf("  -v, --verbose            Print diagnostics to stderr (keeps stdout reports parseable)\n\n");
    printf("EXAMPLES:\n");
    printf("  %s -- ping 127.0.0.1 -n 3\n", prog_name);
    printf("  %s -t 10s -f json -o report.json -- gcc -O3 -c module.c\n", prog_name);
    printf("  %s -n 5 -f human -- my_benchmark.exe --threads 4\n", prog_name);
    printf("  %s \"cmd.exe /c dir /s\"\n\n", prog_name);
}

/**
 * @brief Parses flexible timeout strings (e.g., "500ms", "5s", "2m", or raw numbers).
 *
 * @param str    Input string
 * @param out_ms Receives the timeout in milliseconds (0 disables the timeout)
 * @return 0 on success, -1 if the value is missing, malformed, or out of range
 */
static int parse_timeout(const char *str, uint32_t *out_ms) {
    if (!str || !out_ms || *str == '\0') return -1;

    char *endptr = NULL;
    double val = strtod(str, &endptr);
    if (endptr == str || val < 0.0) return -1;

    while (endptr && isspace((unsigned char)*endptr)) endptr++;

    double multiplier_ms;
    if (*endptr == '\0') {
        /* Default heuristic: values < 100 assumed seconds, >= 100 assumed milliseconds */
        multiplier_ms = (val < 100.0) ? 1000.0 : 1.0;
    } else if (strcmp(endptr, "ms") == 0) {
        multiplier_ms = 1.0;
    } else if (strcmp(endptr, "s") == 0 || strcmp(endptr, "sec") == 0) {
        multiplier_ms = 1000.0;
    } else if (strcmp(endptr, "m") == 0 || strcmp(endptr, "min") == 0) {
        multiplier_ms = 60000.0;
    } else {
        return -1;
    }

    double ms = val * multiplier_ms;
    if (ms < 0.0 || ms > 4294967295.0) return -1;

    *out_ms = (uint32_t)ms;
    return 0;
}

/**
 * @brief Reassembles tokens from argv into a single command line string, quoting as needed.
 *
 * Preserves spaces inside arguments by wrapping tokens containing whitespace in quotes.
 *
 * @param argc       Total argument count
 * @param argv       Argument vector
 * @param start_idx  Index of the first token belonging to the target command
 * @return Dynamically allocated command string (must be freed by caller)
 */
static char* assemble_command(int argc, char *argv[], int start_idx) {
    size_t total_len = 0;
    for (int i = start_idx; i < argc; ++i) {
        total_len += strlen(argv[i]) + 3; /* Allocate space for quotes and spaces */
    }
    if (total_len == 0) return NULL;

    char *cmd = (char*)malloc(total_len + 1);
    if (!cmd) return NULL;
    cmd[0] = '\0';

    for (int i = start_idx; i < argc; ++i) {
        bool needs_quotes = (strchr(argv[i], ' ') != NULL || strchr(argv[i], '\t') != NULL);
        if (i > start_idx) strcat(cmd, " ");
        if (needs_quotes) strcat(cmd, "\"");
        strcat(cmd, argv[i]);
        if (needs_quotes) strcat(cmd, "\"");
    }

    return cmd;
}

/**
 * @brief Main entry point for the CLI executable.
 */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }

    crr_options_t opts;
    crr_options_init(&opts);

    int cmd_start_idx = -1;
    char *dynamic_command = NULL;

    /* Parse command-line options */
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            /* Standard delimiter: everything following is target command */
            cmd_start_idx = i + 1;
            break;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_banner();
            return 0;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--timeout") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", argv[i - 1]);
                return 1;
            }
            if (parse_timeout(argv[i], &opts.timeout_ms) != 0) {
                fprintf(stderr, "Error: Invalid timeout '%s'. Use e.g. 500ms, 5s, or 2m\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--repeat") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", argv[i - 1]);
                return 1;
            }
            char *endptr = NULL;
            long n = strtol(argv[i], &endptr, 10);
            if (endptr == argv[i] || *endptr != '\0' || n < 1 || n > 10000) {
                fprintf(stderr, "Error: Repeat count must be a number between 1 and 10000\n");
                return 1;
            }
            opts.repeat_count = (uint32_t)n;
        } else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--format") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", argv[i - 1]);
                return 1;
            }
            if (strcmp(argv[i], "json") == 0) {
                opts.format = CRR_FORMAT_JSON;
            } else if (strcmp(argv[i], "csv") == 0) {
                opts.format = CRR_FORMAT_CSV;
            } else if (strcmp(argv[i], "human") == 0) {
                opts.format = CRR_FORMAT_HUMAN;
            } else {
                fprintf(stderr, "Error: Unknown format '%s'. Valid options: human, json, csv\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", argv[i - 1]);
                return 1;
            }
            opts.output_file = argv[i];
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: Missing argument for %s\n", argv[i - 1]);
                return 1;
            }
            opts.working_dir = argv[i];
        } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            opts.quiet = true;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--capture") == 0) {
            opts.capture_output = true;
        } else if (strcmp(argv[i], "--no-children") == 0) {
            opts.track_children = false;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            opts.verbose = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unrecognized option '%s'. Use --help for usage.\n", argv[i]);
            return 1;
        } else {
            /* First non-option token initiates the target command string */
            cmd_start_idx = i;
            break;
        }
    }

    if (cmd_start_idx < 0 || cmd_start_idx >= argc) {
        fprintf(stderr, "Error: No command specified to execute.\n");
        fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
        return 1;
    }

    dynamic_command = assemble_command(argc, argv, cmd_start_idx);
    if (!dynamic_command) {
        fprintf(stderr, "Error: Failed to assemble command string.\n");
        return 1;
    }
    opts.command = dynamic_command;

    if (opts.verbose) {
        /* Diagnostics go to stderr so JSON/CSV reports on stdout stay parseable. */
        fprintf(stderr, "[CRR] Target command : %s\n", opts.command);
        fprintf(stderr, "[CRR] Working dir    : %s\n", opts.working_dir ? opts.working_dir : "<current>");
        fprintf(stderr, "[CRR] Timeout        : %u ms%s\n", opts.timeout_ms,
                opts.timeout_ms == 0 ? " (none)" : "");
        fprintf(stderr, "[CRR] Repeat count   : %u\n", opts.repeat_count);
        fprintf(stderr, "[CRR] Track children : %s\n", opts.track_children ? "YES (Job Object)" : "NO");
        fprintf(stderr, "[CRR] Capture output : %s\n", opts.capture_output ? "YES" : "NO");
        fprintf(stderr, "[CRR] Format         : %s\n",
                opts.format == CRR_FORMAT_JSON ? "json" : (opts.format == CRR_FORMAT_CSV ? "csv" : "human"));
    }

    /* ========================================================================= */
    /* SINGLE-RUN EXECUTION PATH                                                 */
    /* ========================================================================= */
    if (opts.repeat_count == 1) {
        crr_metrics_t metrics;
        crr_output_t captured_out;
        memset(&captured_out, 0, sizeof(captured_out));

        uint64_t overhead_ticks_start = process_cpu_ticks_100ns();
        int res = crr_execute(&opts, &metrics, opts.capture_output ? &captured_out : NULL);
        uint64_t overhead_ticks = process_cpu_ticks_100ns() - overhead_ticks_start;

        if (opts.verbose) {
            fprintf(stderr, "[CRR] Result         : status=%s (%d) exit=%d wall=%.6fs rss=%llu B processes=%u\n",
                    crr_status_str(metrics.status_code), (int)metrics.status_code, metrics.exit_code,
                    metrics.wall_time_sec, (unsigned long long)metrics.max_rss_bytes, metrics.process_count);
            fprintf(stderr, "[CRR] Overhead       : ");
            if (overhead_ticks == 0) {
                fprintf(stderr, "< 15.6 ms CRR CPU while supervising (below the Windows timer tick)");
                if (metrics.wall_time_sec > 0.0) {
                    fprintf(stderr, ", < %.2f%% of wall time", 100.0 * 0.015625 / metrics.wall_time_sec);
                }
            } else {
                fprintf(stderr, "%.3f ms CRR CPU while supervising (%llu timer ticks)",
                        (double)overhead_ticks / 10000.0,
                        (unsigned long long)((overhead_ticks + 156249ULL) / 156250ULL));
                if (metrics.wall_time_sec > 0.0) {
                    fprintf(stderr, ", %.2f%% of wall time",
                            100.0 * ((double)overhead_ticks / 10000000.0) / metrics.wall_time_sec);
                }
            }
            fprintf(stderr, "\n");
        }

        if (opts.capture_output) {
            if (captured_out.stdout_buf && captured_out.stdout_len > 0) {
                printf("[Child stdout]:\n%s\n", captured_out.stdout_buf);
            }
            if (captured_out.stderr_buf && captured_out.stderr_len > 0) {
                fprintf(stderr, "[Child stderr]:\n%s\n", captured_out.stderr_buf);
            }
            crr_free_output(&captured_out);
        }

        if (res != CRR_SUCCESS && !metrics.timed_out) {
            fprintf(stderr, "CRR Execution Error: %s (code %d)\n", crr_status_str((crr_status_t)res), res);
            if (res == CRR_ERR_SPAWN_FAILED) {
                fprintf(stderr, "CRR Launch Error: no executable or shell built-in matches '%s'.\n",
                        opts.command);
            }
        }

        /* Format report buffer */
        size_t report_cap = 8192;
        char *report_buf = (char*)malloc(report_cap);
        int report_ok = 0;
        if (!report_buf) {
            fprintf(stderr, "CRR Report Error: memory allocation failed.\n");
            report_ok = -1;
        } else {
            int format_rc = 0;
            if (opts.format == CRR_FORMAT_HUMAN) {
                format_rc = crr_format_human(&metrics, &opts, report_buf, report_cap);
            } else if (opts.format == CRR_FORMAT_JSON) {
                format_rc = crr_format_json(&metrics, &opts, report_buf, report_cap);
            } else if (opts.format == CRR_FORMAT_CSV) {
                format_rc = crr_format_csv(&metrics, &opts, report_buf, report_cap, true);
            }

            if (format_rc != 0) {
                fprintf(stderr, "CRR Report Error: failed to format the report (buffer too small).\n");
                report_ok = -1;
            } else if (crr_write_output(report_buf, opts.output_file) != 0) {
                fprintf(stderr, "CRR Report Error: failed to write the report to '%s'.\n",
                        opts.output_file ? opts.output_file : "<stdout>");
                report_ok = -1;
            }
            free(report_buf);
        }

        free(dynamic_command);
        if (report_ok != 0) return 1;
        if (metrics.timed_out) return 124;
        if (metrics.status_code != CRR_SUCCESS) return 1;
        return metrics.exit_code;
    }

    /* ========================================================================= */
    /* MULTI-RUN BENCHMARK EXECUTION PATH                                        */
    /* ========================================================================= */
    crr_metrics_t *runs = (crr_metrics_t*)malloc(opts.repeat_count * sizeof(crr_metrics_t));
    if (!runs) {
        fprintf(stderr, "Error: Memory allocation failed for benchmark runs.\n");
        free(dynamic_command);
        return 1;
    }

    if (opts.format == CRR_FORMAT_HUMAN && !opts.quiet) {
        printf("Benchmarking command across %u runs: %s\n", opts.repeat_count, opts.command);
    }

    /* Execute the command N times */
    uint64_t overhead_ticks_total = 0;
    for (uint32_t r = 0; r < opts.repeat_count; ++r) {
        if (opts.verbose) {
            fprintf(stderr, "[CRR] Running iteration %u/%u...\n", r + 1, opts.repeat_count);
        }
        uint64_t iter_ticks_start = process_cpu_ticks_100ns();
        int res = crr_execute(&opts, &runs[r], NULL);
        uint64_t iter_ticks = process_cpu_ticks_100ns() - iter_ticks_start;
        overhead_ticks_total += iter_ticks;
        if (opts.verbose) {
            fprintf(stderr, "[CRR] Iteration %u/%u: status=%s (%d) exit=%d wall=%.6fs rss=%llu B overhead=%.3f ms\n",
                    r + 1, opts.repeat_count, crr_status_str(runs[r].status_code),
                    (int)runs[r].status_code, runs[r].exit_code, runs[r].wall_time_sec,
                    (unsigned long long)runs[r].max_rss_bytes, (double)iter_ticks / 10000.0);
        }
        if (res == CRR_ERR_SPAWN_FAILED) {
            fprintf(stderr, "CRR Launch Error: no executable or shell built-in matches '%s'.\n",
                    opts.command);
            free(runs);
            free(dynamic_command);
            return 1;
        }
    }
    if (opts.verbose) {
        fprintf(stderr, "[CRR] Observer overhead: %.3f ms total, %.3f ms/run across %u runs "
                        "(%llu timer ticks)\n",
                (double)overhead_ticks_total / 10000.0,
                (double)overhead_ticks_total / 10000.0 / (double)opts.repeat_count,
                opts.repeat_count,
                (unsigned long long)((overhead_ticks_total + 156249ULL) / 156250ULL));
    }

    /* Calculate descriptive statistics across all runs */
    crr_stats_summary_t summary;
    crr_compute_stats(runs, opts.repeat_count, &summary);

    size_t report_cap = 16384 + (opts.repeat_count * 256);
    char *report_buf = (char*)malloc(report_cap);
    int report_ok = 0;
    if (!report_buf) {
        fprintf(stderr, "CRR Report Error: memory allocation failed.\n");
        report_ok = -1;
    } else {
        int format_rc = 0;
        if (opts.format == CRR_FORMAT_HUMAN) {
            format_rc = crr_format_stats_human(&summary, &opts, report_buf, report_cap);
        } else if (opts.format == CRR_FORMAT_JSON) {
            format_rc = crr_format_stats_json(&summary, runs, &opts, report_buf, report_cap);
        } else if (opts.format == CRR_FORMAT_CSV) {
            format_rc = crr_format_stats_csv(&summary, runs, &opts, report_buf, report_cap);
        }

        if (format_rc != 0) {
            fprintf(stderr, "CRR Report Error: failed to format the benchmark report (buffer too small).\n");
            report_ok = -1;
        } else if (crr_write_output(report_buf, opts.output_file) != 0) {
            fprintf(stderr, "CRR Report Error: failed to write the report to '%s'.\n",
                    opts.output_file ? opts.output_file : "<stdout>");
            report_ok = -1;
        }
        free(report_buf);
    }

    free(runs);
    free(dynamic_command);
    if (report_ok != 0) return 1;
    return (summary.failed_runs > 0) ? 1 : 0;
}
