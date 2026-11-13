/**
 * @file gui_main.c
 * @brief Native Win32 Graphical User Interface (GUI) Control Panel.
 *
 * Implemented 100% in pure C (C11 standard) using native Windows Win32 APIs:
 * 1. Win32 Architecture & Non-Blocking Design:
 *    - The main GUI thread processes window messages (WndProc) continuously,
 *      ensuring the interface never hangs or reports "Not Responding".
 *    - Process execution runs inside a dedicated background worker thread (CreateThread).
 *    - Thread-safe updates are delivered back to the main UI thread via PostMessage
 *      with custom application messages:
 *      * WM_CRR_PROGRESS (live elapsed time & working set memory)
 *      * WM_CRR_FINISHED (run completed / update metrics dashboard)
 *
 * 2. Visual Layout & Common Controls v6:
 *    - Modern Segoe UI interface font for labels, buttons, and inputs.
 *    - Consolas monospaced font for console logs and report output.
 *    - Smooth animated Marquee progress bar indicating ongoing process execution.
 *    - 12-row dual-column ListView control displaying status, wall-clock, CPU times,
 *      utilization percentage, Peak RSS, commit charge, page faults, I/O, and process tree count.
 *
 * 3. User Workflows & Demonstrations:
 *    - One-click preset demonstration buttons (CPU Burn, Mem Burn, Sleep Delay, Child Tree).
 *    - Interactive cancellation via "Cancel" button, terminating runaway processes.
 *    - File export dialogs (GetSaveFileNameA) for exporting metrics directly to JSON or CSV.
 *    - "Copy Report" button copying full text to Windows Clipboard via OpenClipboard/SetClipboardData.
 */

#include "crr.h"
#include "crr_executor.h"
#include "crr_formatter.h"
#include "crr_stats.h"
#include "crr_version.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

/* Custom Application Window Messages */
#define WM_CRR_PROGRESS (WM_USER + 101)
#define WM_CRR_FINISHED (WM_USER + 102)

/* Control Identifiers */
#define IDC_CMD_INPUT          1001
#define IDC_WORKDIR_INPUT      1002
#define IDC_WORKDIR_BROWSE     1003
#define IDC_TIMEOUT_INPUT      1004
#define IDC_REPEAT_INPUT       1005
#define IDC_CHK_TRACK_CHILDREN 1006
#define IDC_CHK_CAPTURE_OUTPUT 1007
#define IDC_BTN_RUN            1008
#define IDC_BTN_CANCEL         1009
#define IDC_BTN_CLEAR          1010
#define IDC_PROGRESS           1011
#define IDC_STATUS_LABEL       1012
#define IDC_BTN_PRESET_CPU     1013
#define IDC_BTN_PRESET_MEM     1014
#define IDC_BTN_PRESET_SLEEP   1015
#define IDC_BTN_PRESET_CHILD   1016
#define IDC_BTN_EXPORT_JSON    1017
#define IDC_BTN_EXPORT_CSV     1018
#define IDC_BTN_COPY_REPORT    1019
#define IDC_METRICS_LIST       1020
#define IDC_OUTPUT_LOG         1021

/**
 * @brief Global application state structure.
 */
typedef struct {
    HWND hwnd;                          /**< Handle to main window */
    crr_options_t opts;                 /**< Active execution options */
    char command_buffer[2048];          /**< Command string buffer */
    char workdir_buffer[1024];          /**< Working directory buffer */

    /* Thread Synchronization */
    HANDLE hThread;                     /**< Worker thread handle */
    volatile bool cancel_requested;     /**< Atomic cancellation flag */
    volatile bool is_running;            /**< Active execution status flag */

    /* Result Containers */
    crr_metrics_t last_metrics;         /**< Single-run measured metrics */
    crr_output_t  captured_output;      /**< Captured child stdout and stderr */
    crr_stats_summary_t last_summary;   /**< Multi-run benchmark statistics summary */
    crr_metrics_t *multi_runs;          /**< Array of individual benchmark runs */
    size_t multi_runs_count;            /**< Total count of benchmark runs */
    char last_human_report[8192];       /**< Formatted human-readable report text */
} gui_state_t;

static gui_state_t g_state;
static HFONT g_hFontUI = NULL;
static HFONT g_hFontMono = NULL;

/**
 * @brief Enables the controls that are only valid while no run is active.
 *
 * Export and Copy act on the report from the previous run; letting them open a
 * dialog mid-benchmark would read half-updated metrics and, because the file
 * dialog runs its own message loop, could stall the worker's completion notice.
 */
static void set_run_controls_enabled(HWND hwnd, bool idle) {
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_RUN), idle);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_CANCEL), !idle);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_EXPORT_JSON), idle);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_EXPORT_CSV), idle);
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_COPY_REPORT), idle);
}

/**
 * @brief Periodic progress callback invoked by the supervisor on the worker thread.
 *
 * Allocates a small telemetry parcel and dispatches it asynchronously via PostMessage.
 */
static void worker_progress_cb(void *user_data, double elapsed_sec, uint64_t current_rss_bytes) {
    gui_state_t *state = (gui_state_t*)user_data;
    if (!state || !state->hwnd) return;

    typedef struct {
        double elapsed;
        uint64_t rss;
    } prog_info_t;

    prog_info_t *info = (prog_info_t*)malloc(sizeof(prog_info_t));
    if (info) {
        info->elapsed = elapsed_sec;
        info->rss = current_rss_bytes;
        PostMessageA(state->hwnd, WM_CRR_PROGRESS, 0, (LPARAM)info);
    }
}

/**
 * @brief Cancellation check callback invoked by the supervisor on each wait iteration.
 */
static bool worker_cancel_cb(void *user_data) {
    gui_state_t *state = (gui_state_t*)user_data;
    return state ? state->cancel_requested : false;
}

/**
 * @brief Background worker thread procedure.
 *
 * Executes the command without blocking the main Win32 message pump.
 */
static DWORD WINAPI worker_thread_proc(LPVOID lpParam) {
    gui_state_t *state = (gui_state_t*)lpParam;

    if (state->opts.repeat_count <= 1) {
        /* Single execution */
        crr_free_output(&state->captured_output);
        crr_execute_with_callbacks(
            &state->opts,
            &state->last_metrics,
            state->opts.capture_output ? &state->captured_output : NULL,
            worker_progress_cb,
            worker_cancel_cb,
            state
        );
        crr_format_human(&state->last_metrics, &state->opts, state->last_human_report, sizeof(state->last_human_report));
    } else {
        /* Multi-run benchmark execution */
        if (state->multi_runs) free(state->multi_runs);
        state->multi_runs = NULL;
        state->multi_runs_count = 0;

        size_t requested = state->opts.repeat_count;
        state->multi_runs = (crr_metrics_t*)calloc(requested, sizeof(crr_metrics_t));
        if (!state->multi_runs) {
            memset(&state->last_summary, 0, sizeof(state->last_summary));
            snprintf(state->last_human_report, sizeof(state->last_human_report),
                     "Benchmark aborted: not enough memory to store %zu runs.\n", requested);
            state->is_running = false;
            PostMessageA(state->hwnd, WM_CRR_FINISHED, 0, 0);
            return 0;
        }

        for (size_t i = 0; i < requested; ++i) {
            if (state->cancel_requested) break;
            crr_execute_with_callbacks(
                &state->opts,
                &state->multi_runs[i],
                NULL,
                worker_progress_cb,
                worker_cancel_cb,
                state
            );
            /* A cancelled run never finished, so it must not enter the sample set. */
            if (state->multi_runs[i].status_code == CRR_ERR_CANCELLED) break;
            state->multi_runs_count = i + 1;
            if (state->multi_runs[i].status_code == CRR_ERR_SPAWN_FAILED) {
                break; /* Repeating a command that cannot be launched is pointless */
            }
        }

        /* Statistics must only cover runs that actually completed: a cancelled
           benchmark leaves the remaining slots untouched. */
        if (state->multi_runs_count > 0 &&
            state->multi_runs[0].status_code == CRR_ERR_SPAWN_FAILED) {
            memset(&state->last_summary, 0, sizeof(state->last_summary));
            snprintf(state->last_human_report, sizeof(state->last_human_report),
                     "Benchmark aborted: no executable or shell built-in matches \"%s\".\n",
                     state->opts.command ? state->opts.command : "");
        } else if (state->multi_runs_count > 0) {
            crr_compute_stats(state->multi_runs, state->multi_runs_count, &state->last_summary);
            crr_format_stats_human(&state->last_summary, &state->opts,
                                   state->last_human_report, sizeof(state->last_human_report));
        } else {
            memset(&state->last_summary, 0, sizeof(state->last_summary));
            snprintf(state->last_human_report, sizeof(state->last_human_report),
                     "Benchmark cancelled before the first run completed.\n");
        }
    }

    state->is_running = false;
    PostMessageA(state->hwnd, WM_CRR_FINISHED, 0, 0);
    return 0;
}

/**
 * @brief Replaces the log text and scrolls back to the first line.
 *
 * An EDIT control keeps its previous scroll offset, so a shorter report written
 * after a longer one can land off-screen and leave the pane looking empty.
 */
static void set_log_text(HWND hLog, const char *text) {
    SetWindowTextA(hLog, text);
    SendMessageA(hLog, EM_SETSEL, 0, 0);
    SendMessageA(hLog, EM_SCROLLCARET, 0, 0);
}

static WNDPROC g_cmdEditProc = NULL;

/**
 * @brief Creation-time geometry of every child control.
 *
 * The GUI is laid out at absolute pixel coordinates. Capturing those rectangles
 * once lets WM_SIZE rescale them proportionally, so the window can be resized
 * without rewriting every coordinate by hand.
 */
typedef struct {
    HWND hwnd;
    RECT design;
} ctl_geom_t;

static ctl_geom_t g_geom[64];
static int g_geom_count = 0;
static int g_design_w = 0;
static int g_design_h = 0;

static BOOL CALLBACK collect_geom_proc(HWND child, LPARAM parent) {
    if (g_geom_count >= (int)(sizeof(g_geom) / sizeof(g_geom[0]))) return FALSE;
    RECT r;
    if (!GetWindowRect(child, &r)) return TRUE;
    POINT tl = { r.left, r.top };
    ScreenToClient((HWND)parent, &tl);
    g_geom[g_geom_count].hwnd = child;
    g_geom[g_geom_count].design.left = tl.x;
    g_geom[g_geom_count].design.top = tl.y;
    g_geom[g_geom_count].design.right = tl.x + (r.right - r.left);
    g_geom[g_geom_count].design.bottom = tl.y + (r.bottom - r.top);
    g_geom_count++;
    return TRUE;
}

static void capture_design_layout(HWND hwnd) {
    RECT client;
    GetClientRect(hwnd, &client);
    g_design_w = client.right;
    g_design_h = client.bottom;
    g_geom_count = 0;
    EnumChildWindows(hwnd, collect_geom_proc, (LPARAM)hwnd);
}

static void layout_children(HWND hwnd) {
    RECT client;
    if (g_geom_count == 0 || g_design_w <= 0 || g_design_h <= 0) return;
    if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0) return;
    double sx = (double)client.right / (double)g_design_w;
    double sy = (double)client.bottom / (double)g_design_h;
    for (int i = 0; i < g_geom_count; ++i) {
        const RECT *d = &g_geom[i].design;
        int x = (int)(d->left * sx + 0.5);
        int y = (int)(d->top * sy + 0.5);
        int w = (int)((d->right - d->left) * sx + 0.5);
        int h = (int)((d->bottom - d->top) * sy + 0.5);
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        MoveWindow(g_geom[i].hwnd, x, y, w, h, TRUE);
    }
}

/**
 * @brief Subclass procedure for the command field: Enter triggers Run.
 *
 * A single-line EDIT control swallows VK_RETURN (it only beeps), so the parent
 * never sees it. Turning WM_CHAR '\r' into a BN_CLICKED notification reuses the
 * exact path the Run button takes, including its enabled/disabled gating.
 */
static LRESULT CALLBACK cmd_edit_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CHAR && wParam == '\r') {
        HWND hParent = GetParent(hwnd);
        HWND hRun = GetDlgItem(hParent, IDC_BTN_RUN);
        if (hRun && IsWindowEnabled(hRun)) {
            SendMessageA(hParent, WM_COMMAND, MAKEWPARAM(IDC_BTN_RUN, BN_CLICKED), (LPARAM)hRun);
        }
        return 0;
    }
    return CallWindowProcA(g_cmdEditProc, hwnd, msg, wParam, lParam);
}

/**
 * @brief Initializes the ListView columns for the metrics dashboard.
 */
static void init_metrics_list(HWND hList) {
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNA col;
    memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = "Resource Metric";
    col.cx = 240;
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    col.pszText = "Measured Value";
    col.cx = 320;
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);
}

/**
 * @brief Adds or sets a row inside the metrics ListView.
 */
static void add_list_item(HWND hList, int index, const char *metric, const char *value) {
    LVITEMA item;
    memset(&item, 0, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = index;
    item.iSubItem = 0;
    item.pszText = (LPSTR)metric;
    ListView_InsertItem(hList, &item);

    ListView_SetItemText(hList, index, 1, (LPSTR)value);
}

/**
 * @brief Converts LF-only text to CRLF for display in a multiline EDIT control.
 *
 * The report formatters emit LF-only line breaks (portable for files, pipes and
 * clipboard consumers). A Win32 EDIT control needs CRLF; with LF alone the whole
 * report collapses into a single visual line. Returns NULL on allocation failure.
 */
static char* crlf_dup(const char *text) {
    if (!text) return NULL;
    size_t len = 0;
    size_t lf_count = 0;
    for (const char *p = text; *p; ++p) {
        ++len;
        if (*p == '\n') ++lf_count;
    }
    if (lf_count > 0 && len > SIZE_MAX - lf_count - 1) return NULL;

    char *out = (char*)malloc(len + lf_count + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (const char *p = text; *p; ++p) {
        if (*p == '\n') out[o++] = '\r';
        out[o++] = *p;
    }
    out[o] = '\0';
    return out;
}

/**
 * @brief Updates the ListView grid and monospaced console with the latest measurements.
 */
static void update_metrics_display(HWND hwnd) {
    HWND hList = GetDlgItem(hwnd, IDC_METRICS_LIST);
    HWND hLog  = GetDlgItem(hwnd, IDC_OUTPUT_LOG);
    ListView_DeleteAllItems(hList);

    if (g_state.opts.repeat_count <= 1) {
        crr_metrics_t *m = &g_state.last_metrics;
        char buf[256];
        char val_buf[512];
        int row = 0;

        /* Row 0: Execution status and exit code */
        const char *status_str = (m->status_code == CRR_SUCCESS) ? "SUCCESS" :
                                 (m->timed_out ? "TIMED OUT" : "FAILED / ERROR");
        snprintf(val_buf, sizeof(val_buf), "%s (Exit Code: %d)", status_str, m->exit_code);
        add_list_item(hList, row++, "Execution Status", val_buf);

        /* Row 1: Wall-Clock Elapsed Time */
        crr_format_time(m->wall_time_sec, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%.6f s  (%s)", m->wall_time_sec, buf);
        add_list_item(hList, row++, "Wall-Clock Time (Real)", val_buf);

        /* Row 2: User Mode CPU Time */
        crr_format_time(m->user_cpu_time_sec, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%.6f s  (%s)", m->user_cpu_time_sec, buf);
        add_list_item(hList, row++, "User CPU Time", val_buf);

        /* Row 3: Kernel / System Mode CPU Time */
        crr_format_time(m->kernel_cpu_time_sec, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%.6f s  (%s)", m->kernel_cpu_time_sec, buf);
        add_list_item(hList, row++, "Kernel / System CPU", val_buf);

        /* Row 4: Total Combined CPU Time */
        crr_format_time(m->total_cpu_time_sec, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%.6f s  (%s)", m->total_cpu_time_sec, buf);
        add_list_item(hList, row++, "Total CPU Time", val_buf);

        /* Row 5: Overall CPU Utilization Percentage */
        snprintf(val_buf, sizeof(val_buf), "%.2f %%", m->cpu_utilization_pct);
        add_list_item(hList, row++, "CPU Utilization", val_buf);

        /* Row 6: Peak Resident Set Size (Peak Working Set) */
        crr_format_bytes(m->max_rss_bytes, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%s  (%" PRIu64 " bytes)", buf, m->max_rss_bytes);
        add_list_item(hList, row++, "Max Resident Set (Peak RSS)", val_buf);

        /* Row 7: Peak Commit Charge / Pagefile Usage */
        crr_format_bytes(m->peak_pagefile_bytes, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%s  (%" PRIu64 " bytes)", buf, m->peak_pagefile_bytes);
        add_list_item(hList, row++, "Peak Commit / Pagefile", val_buf);

        /* Row 8: Page Faults Count */
        snprintf(val_buf, sizeof(val_buf), "%" PRIu64 " faults", m->page_faults);
        add_list_item(hList, row++, "Page Faults", val_buf);

        /* Row 9: File/Device Read I/O */
        crr_format_bytes(m->io_read_bytes, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%s  (%" PRIu64 " ops)", buf, m->io_read_ops);
        add_list_item(hList, row++, "File/Device Read", val_buf);

        /* Row 10: File/Device Write I/O */
        crr_format_bytes(m->io_write_bytes, buf, sizeof(buf));
        snprintf(val_buf, sizeof(val_buf), "%s  (%" PRIu64 " ops)", buf, m->io_write_ops);
        add_list_item(hList, row++, "File/Device Write", val_buf);

        /* Row 11: Process Hierarchy Information */
        snprintf(val_buf, sizeof(val_buf), "%u processes tracked%s",
                 m->process_count, m->child_processes_tracked ? " (Job Object active)" : "");
        add_list_item(hList, row++, "Process Tree Tracking", val_buf);

        /* Update Monospaced Log Console */
        if (g_state.captured_output.stdout_buf || g_state.captured_output.stderr_buf) {
            static const char stdout_header[] = "--- [Child STDOUT] ---\r\n";
            static const char stderr_header[] = "--- [Child STDERR] ---\r\n";
            static const char summary_header[] = "--- [Summary Report] ---\r\n";
            size_t stdout_len = g_state.captured_output.stdout_len;
            size_t stderr_len = g_state.captured_output.stderr_len;
            char *report_disp = crlf_dup(g_state.last_human_report);
            size_t report_len = report_disp ? strlen(report_disp) : 0;
            size_t total_out_len = 1;
            size_t max_size = (size_t)-1;
            bool size_ok = (report_disp != NULL);

#define ADD_OUTPUT_LENGTH(value) \
            do { \
                size_t add_length = (value); \
                if (add_length > max_size - total_out_len) size_ok = false; \
                else total_out_len += add_length; \
            } while (0)
            if (stdout_len > 0) {
                ADD_OUTPUT_LENGTH(sizeof(stdout_header) - 1);
                ADD_OUTPUT_LENGTH(stdout_len);
                ADD_OUTPUT_LENGTH(sizeof("\r\n\r\n") - 1);
            }
            if (stderr_len > 0) {
                ADD_OUTPUT_LENGTH(sizeof(stderr_header) - 1);
                ADD_OUTPUT_LENGTH(stderr_len);
                ADD_OUTPUT_LENGTH(sizeof("\r\n\r\n") - 1);
            }
            ADD_OUTPUT_LENGTH(sizeof(summary_header) - 1);
            ADD_OUTPUT_LENGTH(report_len);
#undef ADD_OUTPUT_LENGTH

            if (size_ok) {
                char *comb = (char*)malloc(total_out_len);
                if (comb) {
                    size_t offset = 0;
                    if (stdout_len > 0) {
                        memcpy(comb + offset, stdout_header, sizeof(stdout_header) - 1);
                        offset += sizeof(stdout_header) - 1;
                        memcpy(comb + offset, g_state.captured_output.stdout_buf, stdout_len);
                        offset += stdout_len;
                        memcpy(comb + offset, "\r\n\r\n", sizeof("\r\n\r\n") - 1);
                        offset += sizeof("\r\n\r\n") - 1;
                    }
                    if (stderr_len > 0) {
                        memcpy(comb + offset, stderr_header, sizeof(stderr_header) - 1);
                        offset += sizeof(stderr_header) - 1;
                        memcpy(comb + offset, g_state.captured_output.stderr_buf, stderr_len);
                        offset += stderr_len;
                        memcpy(comb + offset, "\r\n\r\n", sizeof("\r\n\r\n") - 1);
                        offset += sizeof("\r\n\r\n") - 1;
                    }
                    memcpy(comb + offset, summary_header, sizeof(summary_header) - 1);
                    offset += sizeof(summary_header) - 1;
                    memcpy(comb + offset, report_disp, report_len);
                    comb[offset + report_len] = '\0';
                    set_log_text(hLog, comb);
                    free(comb);
                    free(report_disp);
                } else {
                    SetWindowTextA(hLog, "Unable to allocate memory to display command output.");
                    free(report_disp);
                }
            } else {
                SetWindowTextA(hLog, report_disp ? "Command output is too large to display."
                                                 : "Unable to allocate memory to display command output.");
                free(report_disp);
            }
        } else {
            char *report_disp = crlf_dup(g_state.last_human_report);
            set_log_text(hLog, report_disp ? report_disp : g_state.last_human_report);
            free(report_disp);
        }
    } else {
        /* Multi-run statistical summary view */
        crr_stats_summary_t *s = &g_state.last_summary;
        char val_buf[256];
        char b1[64], b2[64], b3[64];
        int row = 0;

        snprintf(val_buf, sizeof(val_buf), "%zu (Success: %zu, Failed: %zu, Timed Out: %zu)",
                 s->runs_count, s->successful_runs, s->failed_runs, s->timed_out_runs);
        add_list_item(hList, row++, "Completed Benchmark Runs", val_buf);

        snprintf(val_buf, sizeof(val_buf), "Mean: %.4fs | Min: %.4fs | Max: %.4fs | StdDev: %.4fs",
                 s->wall_time.mean, s->wall_time.min, s->wall_time.max, s->wall_time.stddev);
        add_list_item(hList, row++, "Wall-Clock Time Stats", val_buf);

        snprintf(val_buf, sizeof(val_buf), "Mean: %.4fs | Min: %.4fs | Max: %.4fs | StdDev: %.4fs",
                 s->user_cpu.mean, s->user_cpu.min, s->user_cpu.max, s->user_cpu.stddev);
        add_list_item(hList, row++, "User CPU Time Stats", val_buf);

        snprintf(val_buf, sizeof(val_buf), "Mean: %.4fs | Min: %.4fs | Max: %.4fs | StdDev: %.4fs",
                 s->kernel_cpu.mean, s->kernel_cpu.min, s->kernel_cpu.max, s->kernel_cpu.stddev);
        add_list_item(hList, row++, "Kernel CPU Time Stats", val_buf);

        snprintf(val_buf, sizeof(val_buf), "Mean: %.4fs | Min: %.4fs | Max: %.4fs | StdDev: %.4fs",
                 s->total_cpu.mean, s->total_cpu.min, s->total_cpu.max, s->total_cpu.stddev);
        add_list_item(hList, row++, "Total CPU Time Stats", val_buf);

        snprintf(val_buf, sizeof(val_buf), "Mean: %.2f%% | Min: %.2f%% | Max: %.2f%% | StdDev: %.2f%%",
                 s->cpu_utilization.mean, s->cpu_utilization.min, s->cpu_utilization.max, s->cpu_utilization.stddev);
        add_list_item(hList, row++, "CPU Utilization Stats", val_buf);

        crr_format_bytes((uint64_t)s->max_rss_bytes.mean, b1, sizeof(b1));
        crr_format_bytes((uint64_t)s->max_rss_bytes.min, b2, sizeof(b2));
        crr_format_bytes((uint64_t)s->max_rss_bytes.max, b3, sizeof(b3));
        snprintf(val_buf, sizeof(val_buf), "Mean: %s | Min: %s | Max: %s", b1, b2, b3);
        add_list_item(hList, row++, "Max Resident Set Stats", val_buf);

        char *report_disp = crlf_dup(g_state.last_human_report);
        set_log_text(hLog, report_disp ? report_disp : g_state.last_human_report);
        free(report_disp);
    }
}

/**
 * @brief Prompts user with native Save File Dialog and saves report text to disk.
 */
static void prompt_save_file(HWND hwnd, const char *filter, const char *ext, const char *content) {
    if (!content || strlen(content) == 0) {
        MessageBoxA(hwnd, "No data available to export.", "Export Notice", MB_OK | MB_ICONINFORMATION);
        return;
    }

    char file_name[MAX_PATH] = {0};
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file_name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = ext;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameA(&ofn)) {
        FILE *f = fopen(file_name, "w");
        if (f) {
            size_t content_len = strlen(content);
            bool saved = fwrite(content, 1, content_len, f) == content_len;
            if (fclose(f) != 0) {
                saved = false;
            }
            if (saved) {
                MessageBoxA(hwnd, "Export completed successfully!", "Success", MB_OK | MB_ICONINFORMATION);
            } else {
                MessageBoxA(hwnd, "Failed while writing the export file.", "Export Error", MB_OK | MB_ICONERROR);
            }
        } else {
            MessageBoxA(hwnd, "Failed to open file for writing.", "Export Error", MB_OK | MB_ICONERROR);
        }
    } else {
        DWORD dialog_error = CommDlgExtendedError();
        if (dialog_error != 0) {
            char error_message[128];
            snprintf(error_message, sizeof(error_message),
                     "The save dialog failed (error 0x%08lX).", (unsigned long)dialog_error);
            MessageBoxA(hwnd, error_message, "Export Error", MB_OK | MB_ICONERROR);
        }
    }
}

/**
 * @brief Callback for EnumChildWindows to apply application fonts.
 */
static BOOL CALLBACK SetChildFontProc(HWND hChild, LPARAM lParam) {
    (void)lParam;
    int id = GetDlgCtrlID(hChild);
    if (id == IDC_OUTPUT_LOG) {
        SendMessageA(hChild, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
    } else {
        SendMessageA(hChild, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    }
    return TRUE;
}

/**
 * @brief Fills the command box with a workload path resolved against the GUI's
 * own directory, so presets work regardless of the process working directory
 * (for example when the executable is double-clicked from bin\).
 */
static void set_preset_command(HWND hwnd, const char *workload, const char *arguments) {
    char exe_path[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_path, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        char *sep = strrchr(exe_path, '\\');
        if (!sep) sep = strrchr(exe_path, '/');
        if (sep) {
            sep[1] = '\0';
            char cmd[2048];
            int n = snprintf(cmd, sizeof(cmd), "\"%s%s\"%s%s",
                             exe_path, workload,
                             (arguments && *arguments) ? " " : "",
                             arguments ? arguments : "");
            if (n > 0 && (size_t)n < sizeof(cmd)) {
                SetDlgItemTextA(hwnd, IDC_CMD_INPUT, cmd);
                return;
            }
        }
    }
    SetDlgItemTextA(hwnd, IDC_CMD_INPUT, workload);
}

/* Largest timeout the millisecond field can hold once multiplied by 1000. */
#define GUI_MAX_TIMEOUT_SECONDS 4294967u
#define GUI_MAX_REPEAT_COUNT    10000u

/**
 * @brief Parses a GUI numeric field: optional surrounding spaces and decimal
 * digits only. Empty input yields 0. Returns false for signs, letters, or a
 * value above max, so callers can report the field instead of silently
 * coercing it (strtoul would turn "abc" into 0 and "-5" into a huge value).
 */
static bool parse_uint_field(const char *text, uint32_t max, uint32_t *out) {
    if (!text || !out) return false;

    while (*text == ' ' || *text == '\t') text++;
    const char *digits = text;
    uint64_t value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10 + (uint64_t)(*text - '0');
        if (value > max) return false;
        text++;
    }
    while (*text == ' ' || *text == '\t') text++;
    if (*text != '\0') return false;

    *out = (text == digits) ? 0u : (uint32_t)value;
    return true;
}

/**
 * @brief Main Window Procedure handling all OS and user events.
 */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            g_state.hwnd = hwnd;
            crr_options_init(&g_state.opts);

            /* Create clean typography fonts */
            g_hFontUI = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
            g_hFontMono = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

            /* Top Section: Command and Working Directory Controls */
            CreateWindowA("STATIC", "Command to execute:", WS_VISIBLE | WS_CHILD,
                          15, 12, 140, 20, hwnd, NULL, NULL, NULL);
            HWND hCmd = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "ping 127.0.0.1 -n 3",
                                        WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
                                        160, 10, 520, 24, hwnd, (HMENU)IDC_CMD_INPUT, NULL, NULL);
            g_cmdEditProc = (WNDPROC)SetWindowLongPtrA(hCmd, GWLP_WNDPROC, (LONG_PTR)cmd_edit_proc);

            CreateWindowA("STATIC", "Working directory:", WS_VISIBLE | WS_CHILD,
                          15, 42, 140, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "",
                            WS_VISIBLE | WS_CHILD | ES_AUTOHSCROLL,
                            160, 40, 440, 24, hwnd, (HMENU)IDC_WORKDIR_INPUT, NULL, NULL);
            CreateWindowA("BUTTON", "Browse...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          605, 40, 75, 24, hwnd, (HMENU)IDC_WORKDIR_BROWSE, NULL, NULL);

            /* Demonstration Preset Buttons */
            CreateWindowA("STATIC", "Quick Presets:", WS_VISIBLE | WS_CHILD,
                          695, 12, 90, 20, hwnd, NULL, NULL, NULL);
            CreateWindowA("BUTTON", "CPU Burn", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          785, 9, 80, 24, hwnd, (HMENU)IDC_BTN_PRESET_CPU, NULL, NULL);
            CreateWindowA("BUTTON", "Mem Burn", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          870, 9, 80, 24, hwnd, (HMENU)IDC_BTN_PRESET_MEM, NULL, NULL);
            CreateWindowA("BUTTON", "Sleep Delay", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          785, 38, 80, 24, hwnd, (HMENU)IDC_BTN_PRESET_SLEEP, NULL, NULL);
            CreateWindowA("BUTTON", "Child Tree", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          870, 38, 80, 24, hwnd, (HMENU)IDC_BTN_PRESET_CHILD, NULL, NULL);

            /* Options Controls */
            CreateWindowA("STATIC", "Timeout (sec, 0=none):", WS_VISIBLE | WS_CHILD,
                          15, 75, 140, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "0", WS_VISIBLE | WS_CHILD | ES_NUMBER,
                            160, 73, 60, 24, hwnd, (HMENU)IDC_TIMEOUT_INPUT, NULL, NULL);

            CreateWindowA("STATIC", "Repeat count:", WS_VISIBLE | WS_CHILD,
                          235, 75, 90, 20, hwnd, NULL, NULL, NULL);
            CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "1", WS_VISIBLE | WS_CHILD | ES_NUMBER,
                            330, 73, 50, 24, hwnd, (HMENU)IDC_REPEAT_INPUT, NULL, NULL);

            HWND hChkTrack = CreateWindowA("BUTTON", "Track Child Processes (Job Object)",
                                           WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                           400, 74, 250, 22, hwnd, (HMENU)IDC_CHK_TRACK_CHILDREN, NULL, NULL);
            SendMessageA(hChkTrack, BM_SETCHECK, BST_CHECKED, 0);

            HWND hChkCap = CreateWindowA("BUTTON", "Capture Output (stdout/stderr)",
                                         WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                         660, 74, 230, 22, hwnd, (HMENU)IDC_CHK_CAPTURE_OUTPUT, NULL, NULL);
            SendMessageA(hChkCap, BM_SETCHECK, BST_CHECKED, 0);

            /* Action Buttons */
            CreateWindowA("BUTTON", "Run Benchmark / Report", WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                          15, 108, 180, 30, hwnd, (HMENU)IDC_BTN_RUN, NULL, NULL);
            HWND hBtnCancel = CreateWindowA("BUTTON", "Cancel", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                                            205, 108, 90, 30, hwnd, (HMENU)IDC_BTN_CANCEL, NULL, NULL);
            EnableWindow(hBtnCancel, FALSE);

            CreateWindowA("BUTTON", "Clear", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          305, 108, 80, 30, hwnd, (HMENU)IDC_BTN_CLEAR, NULL, NULL);

            /* Export Buttons */
            CreateWindowA("BUTTON", "Export JSON...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          660, 108, 100, 30, hwnd, (HMENU)IDC_BTN_EXPORT_JSON, NULL, NULL);
            CreateWindowA("BUTTON", "Export CSV...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          765, 108, 95, 30, hwnd, (HMENU)IDC_BTN_EXPORT_CSV, NULL, NULL);
            CreateWindowA("BUTTON", "Copy Report", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          865, 108, 85, 30, hwnd, (HMENU)IDC_BTN_COPY_REPORT, NULL, NULL);

            /* Progress Bar and Status Label */
            CreateWindowExA(0, PROGRESS_CLASSA, NULL, WS_VISIBLE | WS_CHILD | PBS_SMOOTH,
                            15, 146, 935, 14, hwnd, (HMENU)IDC_PROGRESS, NULL, NULL);
            CreateWindowA("STATIC", "Status: Ready", WS_VISIBLE | WS_CHILD,
                          15, 164, 935, 20, hwnd, (HMENU)IDC_STATUS_LABEL, NULL, NULL);

            /* Metrics Dashboard Table (Left) */
            HWND hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
                                         WS_VISIBLE | WS_CHILD | LVS_REPORT | LVS_SINGLESEL,
                                         15, 190, 500, 480, hwnd, (HMENU)IDC_METRICS_LIST, NULL, NULL);
            init_metrics_list(hList);

            /* Output and Log Console Viewer (Right) */
            CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "Command Resource Reporter ready.\r\nEnter a command and click 'Run Benchmark / Report' or select a quick preset.",
                            WS_VISIBLE | WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL |
                            ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL | ES_READONLY,
                            525, 190, 425, 480, hwnd, (HMENU)IDC_OUTPUT_LOG, NULL, NULL);

            /* Apply clean fonts to all controls */
            EnumChildWindows(hwnd, SetChildFontProc, 0);
            capture_design_layout(hwnd);

            return 0;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) layout_children(hwnd);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO *mmi = (MINMAXINFO*)lParam;
            RECT min_rect = { 0, 0, 860, 620 };
            AdjustWindowRectEx(&min_rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_APPWINDOW);
            mmi->ptMinTrackSize.x = min_rect.right - min_rect.left;
            mmi->ptMinTrackSize.y = min_rect.bottom - min_rect.top;
            return 0;
        }

        /* Periodic progress update from background worker thread */
        case WM_CRR_PROGRESS: {
            typedef struct { double elapsed; uint64_t rss; } prog_info_t;
            prog_info_t *info = (prog_info_t*)lParam;
            if (info) {
                char stat_buf[256];
                char rss_buf[64];
                crr_format_bytes(info->rss, rss_buf, sizeof(rss_buf));
                snprintf(stat_buf, sizeof(stat_buf), "Status: Executing... (Elapsed: %.2fs | Current RSS: %s)",
                         info->elapsed, rss_buf);
                SetDlgItemTextA(hwnd, IDC_STATUS_LABEL, stat_buf);
                free(info);
            }
            return 0;
        }

        /* Execution completion notification from worker thread */
        case WM_CRR_FINISHED: {
            HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
            SendMessageA(hProg, PBM_SETMARQUEE, FALSE, 0);
            SendMessageA(hProg, PBM_SETPOS, 100, 0);

            set_run_controls_enabled(hwnd, true);

            if (g_state.hThread) {
                CloseHandle(g_state.hThread);
                g_state.hThread = NULL;
            }

            bool spawn_failed = (g_state.opts.repeat_count > 1)
                ? (g_state.multi_runs_count > 0 && g_state.multi_runs &&
                   g_state.multi_runs[0].status_code == CRR_ERR_SPAWN_FAILED)
                : (g_state.last_metrics.status_code == CRR_ERR_SPAWN_FAILED);

            char stat_buf[256];
            if (g_state.cancel_requested) {
                if (g_state.opts.repeat_count > 1) {
                    snprintf(stat_buf, sizeof(stat_buf),
                             "Status: Benchmark cancelled after %zu of %u runs.",
                             g_state.multi_runs_count, g_state.opts.repeat_count);
                } else {
                    snprintf(stat_buf, sizeof(stat_buf), "Status: Execution cancelled by user.");
                }
            } else if (spawn_failed) {
                snprintf(stat_buf, sizeof(stat_buf), "Status: Launch failed - no executable or shell built-in matches the command.");
            } else if (g_state.opts.repeat_count > 1) {
                snprintf(stat_buf, sizeof(stat_buf), "Status: Benchmark complete (%zu runs).", g_state.multi_runs_count);
            } else if (g_state.last_metrics.timed_out) {
                snprintf(stat_buf, sizeof(stat_buf), "Status: Command TIMED OUT after %.2fs.", g_state.last_metrics.wall_time_sec);
            } else {
                snprintf(stat_buf, sizeof(stat_buf), "Status: Completed successfully (Exit: %d) in %.4fs.",
                         g_state.last_metrics.exit_code, g_state.last_metrics.wall_time_sec);
            }
            SetDlgItemTextA(hwnd, IDC_STATUS_LABEL, stat_buf);

            update_metrics_display(hwnd);
            return 0;
        }

        /* User Commands and Button Clicks */
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDC_BTN_PRESET_CPU:
                    set_preset_command(hwnd, "cpu_burn.exe", "250");
                    break;
                case IDC_BTN_PRESET_MEM:
                    set_preset_command(hwnd, "mem_burn.exe", "30");
                    break;
                case IDC_BTN_PRESET_SLEEP:
                    set_preset_command(hwnd, "sleep_delay.exe", "400");
                    break;
                case IDC_BTN_PRESET_CHILD:
                    set_preset_command(hwnd, "child_spawner.exe", "");
                    break;

                case IDC_WORKDIR_BROWSE: {
                    BROWSEINFOA bi;
                    memset(&bi, 0, sizeof(bi));
                    bi.hwndOwner = hwnd;
                    bi.lpszTitle = "Select Working Directory";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
                    if (pidl) {
                        char path[MAX_PATH];
                        if (SHGetPathFromIDListA(pidl, path)) {
                            SetDlgItemTextA(hwnd, IDC_WORKDIR_INPUT, path);
                        }
                        CoTaskMemFree(pidl);
                    }
                    break;
                }

                case IDC_BTN_CLEAR:
                    ListView_DeleteAllItems(GetDlgItem(hwnd, IDC_METRICS_LIST));
                    SetDlgItemTextA(hwnd, IDC_OUTPUT_LOG, "");
                    SetDlgItemTextA(hwnd, IDC_STATUS_LABEL, "Status: Ready");
                    SendMessageA(GetDlgItem(hwnd, IDC_PROGRESS), PBM_SETPOS, 0, 0);
                    break;

                case IDC_BTN_CANCEL:
                    if (g_state.is_running) {
                        g_state.cancel_requested = true;
                        SetDlgItemTextA(hwnd, IDC_STATUS_LABEL, "Status: Terminating child process...");
                    }
                    break;

                case IDC_BTN_RUN: {
                    if (g_state.is_running) break;

                    GetDlgItemTextA(hwnd, IDC_CMD_INPUT, g_state.command_buffer, sizeof(g_state.command_buffer));
                    if (strlen(g_state.command_buffer) == 0) {
                        MessageBoxA(hwnd, "Please enter a command to execute.", "Input Required", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_CMD_INPUT));
                        break;
                    }

                    GetDlgItemTextA(hwnd, IDC_WORKDIR_INPUT, g_state.workdir_buffer, sizeof(g_state.workdir_buffer));

                    char timeout_str[32];
                    GetDlgItemTextA(hwnd, IDC_TIMEOUT_INPUT, timeout_str, sizeof(timeout_str));
                    uint32_t timeout_sec = 0;
                    if (!parse_uint_field(timeout_str, GUI_MAX_TIMEOUT_SECONDS, &timeout_sec)) {
                        MessageBoxA(hwnd,
                                    "Timeout must be a whole number of seconds between 0 and 4294967 "
                                    "(0 means no timeout).",
                                    "Invalid Input", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_TIMEOUT_INPUT));
                        break;
                    }

                    char repeat_str[32];
                    GetDlgItemTextA(hwnd, IDC_REPEAT_INPUT, repeat_str, sizeof(repeat_str));
                    uint32_t repeat_cnt = 0;
                    if (!parse_uint_field(repeat_str, GUI_MAX_REPEAT_COUNT, &repeat_cnt) || repeat_cnt < 1) {
                        MessageBoxA(hwnd,
                                    "Repeat count must be a whole number between 1 and 10000.",
                                    "Invalid Input", MB_OK | MB_ICONWARNING);
                        SetFocus(GetDlgItem(hwnd, IDC_REPEAT_INPUT));
                        break;
                    }

                    g_state.opts.command = g_state.command_buffer;
                    g_state.opts.working_dir = (strlen(g_state.workdir_buffer) > 0) ? g_state.workdir_buffer : NULL;
                    g_state.opts.timeout_ms = timeout_sec * 1000;
                    g_state.opts.repeat_count = repeat_cnt;
                    g_state.opts.track_children = (IsDlgButtonChecked(hwnd, IDC_CHK_TRACK_CHILDREN) == BST_CHECKED);
                    g_state.opts.capture_output = (IsDlgButtonChecked(hwnd, IDC_CHK_CAPTURE_OUTPUT) == BST_CHECKED);

                    g_state.cancel_requested = false;
                    g_state.is_running = true;

                    set_run_controls_enabled(hwnd, false);

                    HWND hProg = GetDlgItem(hwnd, IDC_PROGRESS);
                    SetWindowLongA(hProg, GWL_STYLE, GetWindowLongA(hProg, GWL_STYLE) | PBS_MARQUEE);
                    SendMessageA(hProg, PBM_SETMARQUEE, TRUE, 30);

                    SetDlgItemTextA(hwnd, IDC_STATUS_LABEL, "Status: Spawning process...");

                    /* Launch worker thread */
                    g_state.hThread = CreateThread(NULL, 0, worker_thread_proc, &g_state, 0, NULL);
                    if (!g_state.hThread) {
                        MessageBoxA(hwnd, "Failed to create worker thread.", "System Error", MB_OK | MB_ICONERROR);
                        g_state.is_running = false;
                        set_run_controls_enabled(hwnd, true);
                        SendMessageA(hProg, PBM_SETMARQUEE, FALSE, 0);
                    }
                    break;
                }

                case IDC_BTN_EXPORT_JSON: {
                    if (g_state.is_running) break;
                    char json_buf[16384] = {0};
                    if (g_state.last_human_report[0] == '\0') {
                        MessageBoxA(hwnd, "Run a command before exporting its report.", "Export Notice",
                                    MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    int format_result;
                    if (g_state.opts.repeat_count <= 1) {
                        format_result = crr_format_json(&g_state.last_metrics, &g_state.opts,
                                                        json_buf, sizeof(json_buf));
                    } else {
                        format_result = crr_format_stats_json(&g_state.last_summary, g_state.multi_runs,
                                                              &g_state.opts, json_buf, sizeof(json_buf));
                    }
                    if (format_result != 0) {
                        MessageBoxA(hwnd, "Failed to format the JSON report.", "Export Error",
                                    MB_OK | MB_ICONERROR);
                        break;
                    }
                    prompt_save_file(hwnd, "JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0", "json", json_buf);
                    break;
                }

                case IDC_BTN_EXPORT_CSV: {
                    if (g_state.is_running) break;
                    char csv_buf[16384] = {0};
                    if (g_state.last_human_report[0] == '\0') {
                        MessageBoxA(hwnd, "Run a command before exporting its report.", "Export Notice",
                                    MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    int format_result;
                    if (g_state.opts.repeat_count <= 1) {
                        format_result = crr_format_csv(&g_state.last_metrics, &g_state.opts,
                                                       csv_buf, sizeof(csv_buf), true);
                    } else {
                        format_result = crr_format_stats_csv(&g_state.last_summary, g_state.multi_runs,
                                                             &g_state.opts, csv_buf, sizeof(csv_buf));
                    }
                    if (format_result != 0) {
                        MessageBoxA(hwnd, "Failed to format the CSV report.", "Export Error",
                                    MB_OK | MB_ICONERROR);
                        break;
                    }
                    prompt_save_file(hwnd, "CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0", "csv", csv_buf);
                    break;
                }

                case IDC_BTN_COPY_REPORT: {
                    if (g_state.is_running) break;
                    if (strlen(g_state.last_human_report) == 0) {
                        MessageBoxA(hwnd, "No report available to copy.", "Notice", MB_OK | MB_ICONINFORMATION);
                        break;
                    }
                    if (OpenClipboard(hwnd)) {
                        EmptyClipboard();
                        /* CF_TEXT consumers expect CRLF line breaks */
                        char *report_disp = crlf_dup(g_state.last_human_report);
                        const char *text = report_disp ? report_disp : g_state.last_human_report;
                        size_t len = strlen(text) + 1;
                        bool copied = false;
                        HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, len);
                        if (hGlob) {
                            void *dst = GlobalLock(hGlob);
                            if (dst) {
                                memcpy(dst, text, len);
                                GlobalUnlock(hGlob);
                                copied = (SetClipboardData(CF_TEXT, hGlob) != NULL);
                            }
                            if (!copied) GlobalFree(hGlob);
                        }
                        free(report_disp);
                        CloseClipboard();
                        if (copied) {
                            MessageBoxA(hwnd, "Report copied to clipboard!", "Copied", MB_OK | MB_ICONINFORMATION);
                        } else {
                            MessageBoxA(hwnd, "Could not place the report on the clipboard.", "Clipboard Error",
                                        MB_OK | MB_ICONERROR);
                        }
                    }
                    break;
                }
            }
            return 0;
        }

        /* Clean up resources on window closure */
        case WM_DESTROY: {
            /* Stop the worker before the state it points at is torn down. Waiting
               on the thread handle (rather than on is_running) also covers the
               window between CreateThread returning and the worker's first
               instruction. If the worker will not stop, leave its buffers alone:
               freeing them here would be a use-after-free while it still runs. */
            bool worker_stopped = true;
            if (g_state.hThread) {
                g_state.cancel_requested = true;
                if (WaitForSingleObject(g_state.hThread, 5000) == WAIT_OBJECT_0) {
                    CloseHandle(g_state.hThread);
                    g_state.hThread = NULL;
                } else {
                    worker_stopped = false;
                }
            }

            if (g_hFontUI) DeleteObject(g_hFontUI);
            if (g_hFontMono) DeleteObject(g_hFontMono);
            if (worker_stopped) {
                crr_free_output(&g_state.captured_output);
                if (g_state.multi_runs) free(g_state.multi_runs);
                g_state.multi_runs = NULL;
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

/**
 * @brief WinMain application entry point.
 */
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;

    /* Initialize modern Windows Common Controls v6 */
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    /* Register Window Class */
    WNDCLASSEXA wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "CRR_MainWindowClass";

    if (!RegisterClassExA(&wc)) {
        MessageBoxA(NULL, "Failed to register window class.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    /* Center Window on Primary Display */
    int winWidth = 980;
    int winHeight = 720;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = (screenW - winWidth) / 2;
    int posY = (screenH - winHeight) / 2;

    HWND hwnd = CreateWindowExA(
        WS_EX_APPWINDOW,
        wc.lpszClassName,
        "Command Resource Reporter (CRR) - Control Panel & Benchmark Dashboard",
        WS_OVERLAPPEDWINDOW,
        posX, posY, winWidth, winHeight,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBoxA(NULL, "Failed to create main window.", "Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* Main Win32 Message Loop */
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
