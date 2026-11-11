/**
 * @file crr_executor_win.c
 * @brief Windows-native process execution supervisor and kernel metrics collector.
 *
 * This module implements the process monitoring engine for Microsoft Windows:
 * 1. Process Lifecycle Management:
 *    - Uses CreateProcessA with CREATE_SUSPENDED to eliminate process tree race conditions.
 *    - Wraps commands in cmd.exe /c if direct execution fails (e.g., shell built-ins like dir/echo).
 *
 * 2. Process Tree Accounting via Windows Job Objects:
 *    - Instantiates a kernel Job Object (CreateJobObjectA).
 *    - Assigns the initial process to the Job Object before resuming its primary thread.
 *    - All child and descendant processes spawned by the target inherit Job membership.
 *    - Queries JOBOBJECT_BASIC_ACCOUNTING_INFORMATION for total multi-process CPU time.
 *    - Queries JOBOBJECT_EXTENDED_LIMIT_INFORMATION for peak memory across the entire tree.
 *
 * 3. High-Precision Timing:
 *    - Uses QueryPerformanceFrequency and QueryPerformanceCounter (QPC) for hardware-backed
 *      sub-microsecond wall-clock elapsed time measurement.
 *
 * 4. Memory Profiling via PSAPI:
 *    - Queries GetProcessMemoryInfo for PeakWorkingSetSize (Peak RSS) and PageFaultCount.
 *
 * 5. I/O Accounting:
 *    - Queries GetProcessIoCounters for read/write bytes and operation counts.
 *
 * 6. Deadlock-Free Anonymous Pipe Redirection:
 *    - Uses PeekNamedPipe during polling to drain child stdout/stderr buffers incrementally,
 *      preventing child process deadlocks when pipe buffers fill up.
 */

#include "crr_platform.h"

#if defined(CRR_PLATFORM_WINDOWS)

#include "crr_executor.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Initializes execution options with safe defaults.
 */
void crr_options_init(crr_options_t *opts) {
    if (!opts) return;
    memset(opts, 0, sizeof(*opts));
    opts->timeout_ms = 0;          /* 0 = no timeout (infinite) */
    opts->repeat_count = 1;        /* 1 execution by default */
    opts->track_children = true;   /* Windows Job Object tracking enabled */
    opts->capture_output = false;  /* Direct terminal pass-through by default */
    opts->verbose = false;         /* Minimal logging */
    opts->quiet = false;           /* Standard output not silenced */
    opts->format = CRR_FORMAT_HUMAN;
    opts->output_file = NULL;
}

/**
 * @brief Translates internal error codes into human-readable descriptions.
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
 * @brief Frees memory allocated inside captured output buffers.
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
 * @brief Appends raw bytes to a dynamically expanding heap buffer.
 *
 * @param buf       Pointer to destination buffer pointer
 * @param len       Pointer to current byte length
 * @param cap       Pointer to allocated buffer capacity
 * @param data      Incoming data bytes
 * @param data_len  Number of incoming data bytes
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
    (*buf)[*len] = '\0'; /* Maintain null-terminator for text inspection */
}

/**
 * @brief Reads all currently available data from an anonymous pipe without blocking.
 *
 * Uses PeekNamedPipe to inspect buffer occupancy. If data is present, reads it into
 * the dynamically expanding buffer. If empty, returns immediately.
 */
static void drain_pipe_avail(HANDLE hPipe, char **buf, size_t *len, size_t *cap) {
    if (hPipe == NULL || hPipe == INVALID_HANDLE_VALUE) return;
    DWORD bytes_avail = 0;
    if (!PeekNamedPipe(hPipe, NULL, 0, NULL, &bytes_avail, NULL) || bytes_avail == 0) {
        return;
    }

    char temp[4096];
    while (bytes_avail > 0) {
        DWORD to_read = (bytes_avail > sizeof(temp)) ? (DWORD)sizeof(temp) : bytes_avail;
        DWORD bytes_read = 0;
        if (!ReadFile(hPipe, temp, to_read, &bytes_read, NULL) || bytes_read == 0) {
            break;
        }
        append_to_buffer(buf, len, cap, temp, (size_t)bytes_read);
        bytes_avail -= bytes_read;
    }
}

/**
 * @brief Flushes remaining data from a pipe until end-of-file (handle closure).
 */
static void drain_pipe_eof(HANDLE hPipe, char **buf, size_t *len, size_t *cap) {
    if (hPipe == NULL || hPipe == INVALID_HANDLE_VALUE) return;
    char temp[4096];
    DWORD bytes_read = 0;
    while (ReadFile(hPipe, temp, sizeof(temp), &bytes_read, NULL) && bytes_read > 0) {
        append_to_buffer(buf, len, cap, temp, (size_t)bytes_read);
    }
}

/**
 * @brief Case-insensitive equality for short ASCII tokens.
 */
static bool token_equals_ignore_case(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/**
 * @brief True if the command line contains operators only a shell can interpret.
 */
static bool command_needs_shell(const char *cmd) {
    for (const char *p = cmd; *p; ++p) {
        switch (*p) {
            case '|': case '&': case '<': case '>':
            case '(': case ')': case '^': case '%': case '!':
                return true;
            default:
                break;
        }
    }
    return false;
}

/**
 * @brief Copies the program token (first word, surrounding quotes stripped).
 */
static void extract_program_token(const char *cmd, char *out, size_t out_size) {
    const char *p = cmd;
    size_t i = 0;
    out[0] = '\0';
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '"') {
        ++p;
        while (*p && *p != '"' && i + 1 < out_size) out[i++] = *p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && i + 1 < out_size) out[i++] = *p++;
    }
    out[i] = '\0';
}

/**
 * @brief True if the token names a cmd.exe built-in command.
 */
static bool is_cmd_builtin(const char *name) {
    static const char *const builtins[] = {
        "assoc", "break", "call", "cd", "chdir", "cls", "color", "copy", "date",
        "del", "dir", "echo", "endlocal", "erase", "exit", "for", "ftype", "goto",
        "if", "md", "mkdir", "mklink", "move", "path", "pause", "popd", "prompt",
        "pushd", "rd", "rem", "ren", "rename", "rmdir", "set", "setlocal", "shift",
        "start", "time", "title", "type", "ver", "verify", "vol", NULL
    };
    for (size_t i = 0; builtins[i]; ++i) {
        if (token_equals_ignore_case(name, builtins[i])) return true;
    }
    return false;
}

static bool file_exists(const char *path) {
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/**
 * @brief True if the program token can be executed (existing file or shell built-in).
 *
 * The shell fallback is only meaningful for lines cmd.exe can interpret. Handing it
 * an unresolvable name produces a successful cmd.exe launch that exits 1, which used
 * to be reported as a successful execution of a program that does not exist.
 */
static bool command_can_run(const char *token, const char *working_dir) {
    if (token[0] == '\0') return false;
    if (is_cmd_builtin(token)) return true;

    static const char *const extensions[] = { "", ".exe", ".com", ".bat", ".cmd" };
    bool has_directory = strchr(token, '\\') || strchr(token, '/') || strchr(token, ':');
    char candidate[MAX_PATH * 2];

    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        snprintf(candidate, sizeof(candidate), "%s%s", token, extensions[i]);
        if (has_directory) {
            if (file_exists(candidate)) return true;
        } else {
            char resolved[MAX_PATH];
            if (SearchPathA(NULL, candidate, NULL, (DWORD)sizeof(resolved), resolved, NULL) > 0) {
                return true;
            }
            /* cmd.exe also resolves relative programs against the child's working dir */
            if (working_dir && working_dir[0]) {
                snprintf(candidate, sizeof(candidate), "%s\\%s%s", working_dir, token, extensions[i]);
                if (file_exists(candidate)) return true;
            }
        }
    }
    return false;
}

/**
 * @brief Executes a command on Windows, manages Job Objects, and retrieves resource metrics.
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

    /* With capture enabled but no caller sink (e.g. benchmark mode), drain into
       a local discard buffer. Leaving the pipes unread blocks a verbose child. */
    crr_output_t discard;
    crr_output_t *capture = output;
    if (opts->capture_output && !capture) {
        memset(&discard, 0, sizeof(discard));
        capture = &discard;
    }

    /* Step 1: Initialize high-resolution performance counter */
    LARGE_INTEGER qpc_freq, start_time, end_time;
    if (!QueryPerformanceFrequency(&qpc_freq)) {
        return CRR_ERR_METRICS_FAILED;
    }

    /* Step 2: Create Windows Job Object for process tree monitoring */
    HANDLE hJob = NULL;
    if (opts->track_children) {
        hJob = CreateJobObjectA(NULL, NULL);
    }

    /* Step 3: Configure I/O redirection pipes */
    HANDLE hStdOutRead = NULL, hStdOutWrite = NULL;
    HANDLE hStdErrRead = NULL, hStdErrWrite = NULL;
    HANDLE hNullFile = NULL;
    size_t stdout_cap = 0, stderr_cap = 0;

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE; /* Allow child to inherit write ends of pipes */
    sa.lpSecurityDescriptor = NULL;

    STARTUPINFOA si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);

    if (opts->capture_output) {
        if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0) ||
            !CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0)) {
            if (hJob) CloseHandle(hJob);
            return CRR_ERR_IO;
        }
        /* Ensure the parent's read ends are NOT inherited by child */
        SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);

        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = hStdOutWrite;
        si.hStdError  = hStdErrWrite;
    } else if (opts->quiet) {
        /* Discard output by redirecting to NUL */
        hNullFile = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                                &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hNullFile != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = hNullFile;
            si.hStdError  = hNullFile;
        }
    }

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    /* Create mutable copy of command string required by CreateProcessA */
    size_t cmd_len = strlen(opts->command);
    char *cmd_buf = (char*)malloc(cmd_len + 1);
    if (!cmd_buf) {
        if (hJob) CloseHandle(hJob);
        if (hStdOutRead) CloseHandle(hStdOutRead);
        if (hStdOutWrite) CloseHandle(hStdOutWrite);
        if (hStdErrRead) CloseHandle(hStdErrRead);
        if (hStdErrWrite) CloseHandle(hStdErrWrite);
        if (hNullFile && hNullFile != INVALID_HANDLE_VALUE) CloseHandle(hNullFile);
        return CRR_ERR_MEMORY;
    }
    memcpy(cmd_buf, opts->command, cmd_len + 1);

    /* Step 4: Launch child process in SUSPENDED state */
    DWORD creation_flags = CREATE_SUSPENDED;

    /* A windowless supervisor (the GUI) makes Windows allocate a console for a
       console-subsystem child; the default terminal host can fail that
       allocation with ERROR_NO_SYSTEM_RESOURCES (0x800700E8). When output is
       already redirected, CREATE_NO_WINDOW keeps the child's console hidden. */
    if ((opts->capture_output || opts->quiet) && GetConsoleWindow() == NULL) {
        creation_flags |= CREATE_NO_WINDOW;
    }

    /* Start wall-clock timer immediately before process launch */
    QueryPerformanceCounter(&start_time);

    BOOL created = CreateProcessA(
        NULL,
        cmd_buf,
        NULL,
        NULL,
        TRUE,
        creation_flags,
        NULL,
        opts->working_dir,
        &si,
        &pi
    );

    /* Fallback: If direct execution fails, wrap command in cmd.exe /c (e.g. built-ins) */
    if (!created) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND || err == ERROR_BAD_EXE_FORMAT) {
            char token[1024];
            extract_program_token(opts->command, token, sizeof(token));
            if (command_needs_shell(opts->command) ||
                command_can_run(token, opts->working_dir)) {
                size_t wrapped_len = cmd_len + 32;
                char *wrapped_cmd = (char*)malloc(wrapped_len);
                if (wrapped_cmd) {
                    snprintf(wrapped_cmd, wrapped_len, "cmd.exe /c \"%s\"", opts->command);
                    created = CreateProcessA(
                        NULL,
                        wrapped_cmd,
                        NULL,
                        NULL,
                        TRUE,
                        creation_flags,
                        NULL,
                        opts->working_dir,
                        &si,
                        &pi
                    );
                    free(wrapped_cmd);
                }
            }
        }
    }

    free(cmd_buf);

    /* Close write pipe handles in parent so child holds the only open write handles */
    if (hStdOutWrite) { CloseHandle(hStdOutWrite); hStdOutWrite = NULL; }
    if (hStdErrWrite) { CloseHandle(hStdErrWrite); hStdErrWrite = NULL; }
    if (hNullFile && hNullFile != INVALID_HANDLE_VALUE) {
        CloseHandle(hNullFile);
        hNullFile = NULL;
    }

    if (!created) {
        if (hJob) CloseHandle(hJob);
        if (hStdOutRead) CloseHandle(hStdOutRead);
        if (hStdErrRead) CloseHandle(hStdErrRead);
        metrics->status_code = CRR_ERR_SPAWN_FAILED;
        return CRR_ERR_SPAWN_FAILED;
    }

    /* Step 5: Assign process to Job Object BEFORE resuming to capture all descendants */
    if (hJob) {
        AssignProcessToJobObject(hJob, pi.hProcess);
    }

    /* Step 6: Resume main thread to begin active execution */
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread); /* Thread handle no longer needed */

    bool cancelled = false;
    bool timed_out = false;

    /* Step 7: Polling wait loop (25 ms slices) */
    while (true) {
        /* Check for user cancellation (e.g. GUI Cancel button) */
        if (cancel_fn && cancel_fn(user_data)) {
            cancelled = true;
            break;
        }

        /* Check for timeout expiration */
        LARGE_INTEGER cur_time;
        QueryPerformanceCounter(&cur_time);
        double elapsed_sec = (double)(cur_time.QuadPart - start_time.QuadPart) / (double)qpc_freq.QuadPart;

        if (opts->timeout_ms > 0 && (elapsed_sec * 1000.0) >= opts->timeout_ms) {
            timed_out = true;
            break;
        }

        /* Non-blocking pipe drainage to avoid deadlocks */
        if (opts->capture_output && capture) {
            drain_pipe_avail(hStdOutRead, &capture->stdout_buf, &capture->stdout_len, &stdout_cap);
            drain_pipe_avail(hStdErrRead, &capture->stderr_buf, &capture->stderr_len, &stderr_cap);
        }

        /* Dispatch periodic progress telemetry */
        if (progress_cb) {
            PROCESS_MEMORY_COUNTERS cur_pmc;
            uint64_t cur_rss = 0;
            if (GetProcessMemoryInfo(pi.hProcess, &cur_pmc, sizeof(cur_pmc))) {
                cur_rss = (uint64_t)cur_pmc.WorkingSetSize;
            }
            progress_cb(user_data, elapsed_sec, cur_rss);
        }

        /* Wait for process termination with short 25ms timeout */
        DWORD wait_res = WaitForSingleObject(pi.hProcess, 25);
        if (wait_res == WAIT_OBJECT_0) {
            break; /* Process terminated */
        } else if (wait_res == WAIT_FAILED) {
            metrics->status_code = CRR_ERR_WAIT_FAILED;
            break;
        }
    }

    /* Step 8: Handle timeout or cancellation cleanup */
    if (cancelled || timed_out) {
        if (hJob) {
            /* TerminateJobObject terminates all processes in the tree simultaneously */
            TerminateJobObject(hJob, 1);
        }
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 500);
        metrics->timed_out = timed_out;
        metrics->status_code = timed_out ? CRR_ERR_TIMED_OUT : CRR_ERR_CANCELLED;
    }

    /* Step 9: Finalize wall-clock elapsed time */
    QueryPerformanceCounter(&end_time);
    metrics->wall_time_sec = (double)(end_time.QuadPart - start_time.QuadPart) / (double)qpc_freq.QuadPart;

    /* Step 10: Drain any remaining pipe output until EOF */
    if (opts->capture_output && capture) {
        drain_pipe_eof(hStdOutRead, &capture->stdout_buf, &capture->stdout_len, &stdout_cap);
        drain_pipe_eof(hStdErrRead, &capture->stderr_buf, &capture->stderr_len, &stderr_cap);
    }
    if (hStdOutRead) CloseHandle(hStdOutRead);
    if (hStdErrRead) CloseHandle(hStdErrRead);
    if (capture == &discard) crr_free_output(&discard);

    /* Step 11: Retrieve Process Exit Code */
    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        metrics->exit_code = (int)exit_code;
    }

    /* Step 12: Query User and Kernel CPU times */
    FILETIME ftCreate, ftExit, ftKernel, ftUser;
    if (GetProcessTimes(pi.hProcess, &ftCreate, &ftExit, &ftKernel, &ftUser)) {
        ULARGE_INTEGER uUser, uKernel;
        uUser.LowPart = ftUser.dwLowDateTime;
        uUser.HighPart = ftUser.dwHighDateTime;
        uKernel.LowPart = ftKernel.dwLowDateTime;
        uKernel.HighPart = ftKernel.dwHighDateTime;

        /* FILETIME values are in 100-nanosecond units (10,000,000 units = 1 second) */
        metrics->user_cpu_time_sec = (double)uUser.QuadPart / 10000000.0;
        metrics->kernel_cpu_time_sec = (double)uKernel.QuadPart / 10000000.0;
    }

    /* Step 13: Query Memory Statistics via PSAPI */
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof(pmc));
    if (GetProcessMemoryInfo(pi.hProcess, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        metrics->max_rss_bytes       = (uint64_t)pmc.PeakWorkingSetSize;
        metrics->peak_pagefile_bytes = (uint64_t)pmc.PeakPagefileUsage;
        metrics->page_faults         = (uint64_t)pmc.PageFaultCount;
    }

    /* Step 14: Query I/O Transfer Counters */
    IO_COUNTERS io;
    memset(&io, 0, sizeof(io));
    if (GetProcessIoCounters(pi.hProcess, &io)) {
        metrics->io_read_bytes  = (uint64_t)io.ReadTransferCount;
        metrics->io_write_bytes = (uint64_t)io.WriteTransferCount;
        metrics->io_read_ops    = (uint64_t)io.ReadOperationCount;
        metrics->io_write_ops   = (uint64_t)io.WriteOperationCount;
    }

    metrics->process_count = 1;

    /* Step 15: Aggregate Job Object statistics across all descendant processes */
    if (hJob) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION jobBasic;
        memset(&jobBasic, 0, sizeof(jobBasic));
        if (QueryInformationJobObject(hJob, JobObjectBasicAccountingInformation, &jobBasic, sizeof(jobBasic), NULL)) {
            metrics->child_processes_tracked = true;
            metrics->process_count = (uint32_t)jobBasic.TotalProcesses;

            if ((uint64_t)jobBasic.TotalPageFaultCount > metrics->page_faults) {
                metrics->page_faults = (uint64_t)jobBasic.TotalPageFaultCount;
            }

            double jobUser   = (double)jobBasic.TotalUserTime.QuadPart / 10000000.0;
            double jobKernel = (double)jobBasic.TotalKernelTime.QuadPart / 10000000.0;

            if (jobUser > metrics->user_cpu_time_sec) {
                metrics->user_cpu_time_sec = jobUser;
            }
            if (jobKernel > metrics->kernel_cpu_time_sec) {
                metrics->kernel_cpu_time_sec = jobKernel;
            }
        }

        /* The job totals include the root process, so they are the tree-wide values. */
        JOBOBJECT_BASIC_AND_IO_ACCOUNTING_INFORMATION jobIo;
        memset(&jobIo, 0, sizeof(jobIo));
        if (QueryInformationJobObject(hJob, JobObjectBasicAndIoAccountingInformation, &jobIo, sizeof(jobIo), NULL)) {
            if (jobIo.IoInfo.ReadTransferCount > metrics->io_read_bytes) {
                metrics->io_read_bytes = (uint64_t)jobIo.IoInfo.ReadTransferCount;
                metrics->io_read_ops   = (uint64_t)jobIo.IoInfo.ReadOperationCount;
            }
            if (jobIo.IoInfo.WriteTransferCount > metrics->io_write_bytes) {
                metrics->io_write_bytes = (uint64_t)jobIo.IoInfo.WriteTransferCount;
                metrics->io_write_ops   = (uint64_t)jobIo.IoInfo.WriteOperationCount;
            }
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobExt;
        memset(&jobExt, 0, sizeof(jobExt));
        if (QueryInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jobExt, sizeof(jobExt), NULL)) {
            if ((uint64_t)jobExt.PeakJobMemoryUsed > metrics->max_rss_bytes) {
                metrics->max_rss_bytes = (uint64_t)jobExt.PeakJobMemoryUsed;
            }
        }
        CloseHandle(hJob);
    }

    CloseHandle(pi.hProcess);

    /* Step 16: Compute Total CPU time and CPU Utilization Percentage */
    metrics->total_cpu_time_sec = metrics->user_cpu_time_sec + metrics->kernel_cpu_time_sec;
    if (metrics->wall_time_sec > 0.0) {
        metrics->cpu_utilization_pct = (metrics->total_cpu_time_sec / metrics->wall_time_sec) * 100.0;
    } else {
        metrics->cpu_utilization_pct = 0.0;
    }

    if (!timed_out && !cancelled && metrics->status_code == CRR_SUCCESS) {
        metrics->status_code = CRR_SUCCESS;
    }

    return metrics->status_code;
}

/**
 * @brief Simple execution wrapper without callbacks.
 */
int crr_execute(const crr_options_t *opts, crr_metrics_t *metrics, crr_output_t *output) {
    return crr_execute_with_callbacks(opts, metrics, output, NULL, NULL, NULL);
}

#endif /* CRR_PLATFORM_WINDOWS */
