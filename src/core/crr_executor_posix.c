/**
 * @file crr_executor_posix.c
 * @brief POSIX-compliant process execution supervisor and kernel metrics collector.
 *
 * This module provides the cross-platform POSIX implementation of the CRR execution engine:
 * 1. Process Spawning:
 *    - Uses fork() and execl("/bin/sh", "sh", "-c", command, NULL).
 *    - Performs working directory changes (chdir) in child before exec.
 *
 * 2. High-Resolution Timing:
 *    - Uses clock_gettime(CLOCK_MONOTONIC, &ts) for wall-clock elapsed time,
 *      providing nanosecond-level resolution immune to system clock adjustments.
 *
 * 3. Resource Accounting:
 *    - Uses wait4(pid, &wstatus, WNOHANG, &usage) to retrieve the struct rusage:
 *      * ru_utime: User mode CPU time (seconds + microseconds).
 *      * ru_stime: System/Kernel mode CPU time.
 *      * ru_maxrss: Peak resident set size (KiB on Linux, Bytes on BSD/macOS).
 *      * ru_minflt + ru_majflt: Soft (minor) and hard (major) page faults.
 *      * ru_inblock / ru_oublock: Block I/O operations.
 *
 * 4. Interprocess Communication & Pipe Drainage:
 *    - Uses pipe() and non-blocking I/O (O_NONBLOCK via fcntl) to capture output
 *      without risking child execution deadlocks.
 *
 * 5. Process Termination:
 *    - Dispatches SIGKILL to child on timeout or cancellation, followed by wait4
 *      to ensure the child does not become a persistent zombie process.
 */

#include "crr_platform.h"

#if defined(CRR_PLATFORM_POSIX)

#include "crr_executor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

/**
 * @brief Initializes execution options with safe defaults.
 */
void crr_options_init(crr_options_t *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->timeout_ms = 0;
    opts->repeat_count = 1;
    opts->track_children = true;
    opts->capture_output = false;
    opts->verbose = false;
    opts->quiet = false;
    opts->format = CRR_FORMAT_HUMAN;
    opts->output_file = NULL;
}

/**
 * @brief Translates status code into human-readable string.
 */
const char* crr_status_str(crr_status_t status) {
    switch (status) {
        case CRR_SUCCESS:            return "Success";
        case CRR_ERR_INVALID_ARGS:   return "Invalid arguments";
        case CRR_ERR_SPAWN_FAILED:   return "Process spawn failed";
        case CRR_ERR_TIMED_OUT:      return "Execution timed out";
        case CRR_ERR_WAIT_FAILED:    return "Wait on process failed";
        case CRR_ERR_METRICS_FAILED: return "Resource metrics retrieval failed";
        case CRR_ERR_IO:             return "I/O error";
        case CRR_ERR_MEMORY:         return "Memory allocation error";
        case CRR_ERR_CANCELLED:      return "Execution cancelled by user";
        default:                     return "Unknown error";
    }
}

/**
 * @brief Releases memory allocated inside captured output buffers.
 */
void crr_free_output(crr_output_t *output) {
    if (!output) return;
    if (output->stdout_buf) {
        free(output->stdout_buf);
        output->stdout_buf = NULL;
    }
    output->stdout_len = 0;
    if (output->stderr_buf) {
        free(output->stderr_buf);
        output->stderr_buf = NULL;
    }
    output->stderr_len = 0;
}

/**
 * @brief Dynamically reallocates and appends data to a heap buffer.
 */
static void append_to_buffer(char **buf, size_t *len, size_t *cap, const char *data, size_t data_len) {
    if (data_len == 0) return;
    size_t needed = *len + data_len + 1;
    if (needed > *cap) {
        size_t new_cap = (*cap == 0) ? 4096 : (*cap * 2);
        while (new_cap < needed) new_cap *= 2;
        char *new_buf = (char*)realloc(*buf, new_cap);
        if (!new_buf) return;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    (*buf)[*len] = '\0';
}

/**
 * @brief Drains available data from a non-blocking POSIX file descriptor.
 */
static void drain_fd_nonblocking(int fd, char **buf, size_t *len, size_t *cap) {
    if (fd < 0) return;
    char temp[4096];
    while (1) {
        ssize_t n = read(fd, temp, sizeof(temp));
        if (n > 0) {
            append_to_buffer(buf, len, cap, temp, (size_t)n);
        } else {
            break;
        }
    }
}

/**
 * @brief Computes elapsed time in seconds between two struct timespec values.
 */
static double timespec_diff(const struct timespec *start, const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) + (double)(end->tv_nsec - start->tv_nsec) / 1e9;
}

/**
 * @brief Executes command on POSIX systems, monitors execution, and collects rusage metrics.
 */
int crr_execute_with_callbacks(const crr_options_t *opts,
                               crr_metrics_t *metrics,
                               crr_output_t *output,
                               crr_progress_cb progress_cb,
                               crr_cancel_check_fn cancel_fn,
                               void *user_data) {
    if (!opts || !opts->command || strlen(opts->command) == 0 || !metrics) {
        return CRR_ERR_INVALID_ARGS;
    }

    memset(metrics, 0, sizeof(*metrics));
    if (output) {
        output->stdout_buf = NULL;
        output->stdout_len = 0;
        output->stderr_buf = NULL;
        output->stderr_len = 0;
    }

    /* With capture enabled but no caller sink, drain into a local discard buffer
       so a verbose child never blocks on a full pipe. */
    crr_output_t discard;
    crr_output_t *capture = output;
    if (opts->capture_output && !capture) {
        memset(&discard, 0, sizeof(discard));
        capture = &discard;
    }

    int pipe_out[2] = {-1, -1};
    int pipe_err[2] = {-1, -1};
    size_t stdout_cap = 0, stderr_cap = 0;

    /* Set up POSIX anonymous pipes if output capture is requested */
    if (opts->capture_output) {
        if (pipe(pipe_out) < 0 || pipe(pipe_err) < 0) {
            if (pipe_out[0] >= 0) { close(pipe_out[0]); close(pipe_out[1]); }
            return CRR_ERR_IO;
        }
        fcntl(pipe_out[0], F_SETFL, O_NONBLOCK);
        fcntl(pipe_err[0], F_SETFL, O_NONBLOCK);
    }

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Fork child process */
    pid_t pid = fork();
    if (pid < 0) {
        if (opts->capture_output) {
            close(pipe_out[0]); close(pipe_out[1]);
            close(pipe_err[0]); close(pipe_err[1]);
        }
        metrics->status_code = CRR_ERR_SPAWN_FAILED;
        return CRR_ERR_SPAWN_FAILED;
    }

    if (pid == 0) {
        /* Inside Child Process */
        if (opts->working_dir && strlen(opts->working_dir) > 0) {
            if (chdir(opts->working_dir) != 0) {
                _exit(127);
            }
        }

        if (opts->capture_output) {
            close(pipe_out[0]);
            close(pipe_err[0]);
            dup2(pipe_out[1], STDOUT_FILENO);
            dup2(pipe_err[1], STDERR_FILENO);
            close(pipe_out[1]);
            close(pipe_err[1]);
        } else if (opts->quiet) {
            int null_fd = open("/dev/null", O_WRONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDOUT_FILENO);
                dup2(null_fd, STDERR_FILENO);
                close(null_fd);
            }
        }

        /* Execute command via standard POSIX shell */
        execl("/bin/sh", "sh", "-c", opts->command, (char*)NULL);
        _exit(127); /* If exec fails */
    }

    /* Inside Parent Process */
    if (opts->capture_output) {
        close(pipe_out[1]);
        close(pipe_err[1]);
    }

    bool cancelled = false;
    bool timed_out = false;
    int wstatus = 0;
    struct rusage usage;
    memset(&usage, 0, sizeof(usage));

    /* Monitoring loop (20 ms polling) */
    while (1) {
        if (cancel_fn && cancel_fn(user_data)) {
            cancelled = true;
            break;
        }

        struct timespec cur_time;
        clock_gettime(CLOCK_MONOTONIC, &cur_time);
        double elapsed = timespec_diff(&start_time, &cur_time);

        if (opts->timeout_ms > 0 && (elapsed * 1000.0) >= opts->timeout_ms) {
            timed_out = true;
            break;
        }

        if (opts->capture_output && capture) {
            drain_fd_nonblocking(pipe_out[0], &capture->stdout_buf, &capture->stdout_len, &stdout_cap);
            drain_fd_nonblocking(pipe_err[0], &capture->stderr_buf, &capture->stderr_len, &stderr_cap);
        }

        if (progress_cb) {
            progress_cb(user_data, elapsed, 0);
        }

        /* Non-blocking wait for child status and resource usage */
        pid_t res = wait4(pid, &wstatus, WNOHANG, &usage);
        if (res == pid) {
            break; /* Process exited */
        } else if (res < 0) {
            metrics->status_code = CRR_ERR_WAIT_FAILED;
            break;
        }

        usleep(20000); /* Sleep 20 ms */
    }

    /* Handle timeout or cancellation by terminating child with SIGKILL */
    if (cancelled || timed_out) {
        kill(pid, SIGKILL);
        wait4(pid, &wstatus, 0, &usage); /* Reap zombie */
        metrics->timed_out = timed_out;
        metrics->status_code = timed_out ? CRR_ERR_TIMED_OUT : CRR_ERR_CANCELLED;
    }

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    metrics->wall_time_sec = timespec_diff(&start_time, &end_time);

    /* Drain remainder of pipes */
    if (opts->capture_output && capture) {
        drain_fd_nonblocking(pipe_out[0], &capture->stdout_buf, &capture->stdout_len, &stdout_cap);
        drain_fd_nonblocking(pipe_err[0], &capture->stderr_buf, &capture->stderr_len, &stderr_cap);
        close(pipe_out[0]);
        close(pipe_err[0]);
    }
    if (capture == &discard) crr_free_output(&discard);

    /* Extract exit status or signal */
    if (WIFEXITED(wstatus)) {
        metrics->exit_code = WEXITSTATUS(wstatus);
    } else if (WIFSIGNALED(wstatus)) {
        metrics->terminated_by_signal = true;
        metrics->term_signal = WTERMSIG(wstatus);
        metrics->exit_code = 128 + metrics->term_signal;
    }

    /* Convert struct rusage values to standard metrics */
    metrics->user_cpu_time_sec = (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6;
    metrics->kernel_cpu_time_sec = (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
    metrics->total_cpu_time_sec = metrics->user_cpu_time_sec + metrics->kernel_cpu_time_sec;

    /* Linux ru_maxrss is in KiB; macOS/BSD is in Bytes */
#if defined(__APPLE__) || defined(__FreeBSD__)
    metrics->max_rss_bytes = (uint64_t)usage.ru_maxrss;
#else
    metrics->max_rss_bytes = (uint64_t)usage.ru_maxrss * 1024ULL;
#endif

    metrics->page_faults = (uint64_t)(usage.ru_minflt + usage.ru_majflt);
    metrics->io_read_ops = (uint64_t)usage.ru_inblock;
    metrics->io_write_ops = (uint64_t)usage.ru_oublock;
    metrics->process_count = 1;
    metrics->child_processes_tracked = false;

    if (metrics->wall_time_sec > 0.0) {
        metrics->cpu_utilization_pct = (metrics->total_cpu_time_sec / metrics->wall_time_sec) * 100.0;
    }

    if (!timed_out && !cancelled && metrics->status_code == CRR_SUCCESS) {
        metrics->status_code = CRR_SUCCESS;
    }

    return metrics->status_code;
}

/**
 * @brief Simple execution wrapper.
 */
int crr_execute(const crr_options_t *opts, crr_metrics_t *metrics, crr_output_t *output) {
    return crr_execute_with_callbacks(opts, metrics, output, NULL, NULL, NULL);
}

#endif /* CRR_PLATFORM_POSIX */
