# Command Resource Reporter (CRR)

[![Language](https://img.shields.io/badge/Language-C11-00599C.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20POSIX-blue.svg)](https://en.wikipedia.org/wiki/Microsoft_Windows)
[![Build](https://img.shields.io/badge/Build-GCC%20%7C%20Make-success.svg)](#building-the-project)
[![Tests](https://img.shields.io/badge/Tests-66%2F66%20Passing-brightgreen.svg)](#testing-and-validation)

> **Systems Programming Tool (Intermediate Level)**: A high-precision systems utility implemented strictly in the **C Programming Language** that executes any arbitrary command and measures its exact operating-system resource consumption, including high-resolution wall-clock elapsed time, user and kernel CPU execution time, CPU utilization percentage, peak resident set size (Peak Working Set / Max RSS), page fault counts, file/device I/O counters, and recursive child process tree accounting via Windows Job Objects.

---

## Table of Contents
1. [Project Overview](#project-overview)
2. [Key Features](#key-features)
3. [Architecture and Design](#architecture-and-design)
4. [Project Directory Layout](#project-directory-layout)
5. [Prerequisites](#prerequisites)
6. [Building the Project](#building-the-project)
7. [Running the CLI Application](#running-the-cli-application)
8. [Running the GUI Control Panel](#running-the-gui-control-panel)
9. [Testing and Validation](#testing-and-validation)
10. [Sample Output](#sample-output)
11. [Known Limitations](#known-limitations)

---

## 1. Project Overview

Performance optimization and systems troubleshooting require exact empirical measurements of resource consumption. While standard command line shells report execution success or failure, they do not provide detailed systems-level metrics regarding memory footprints, operating system page faults, kernel context switches, or disk transfer rates.

**Command Resource Reporter (CRR)** bridges this gap. It acts as a transparent, high-precision process supervisor that spawns target commands, attaches native operating system accounting facilities, monitors execution asynchronously, and serializes comprehensive resource reports in Human-readable, JSON, and CSV formats.

---

## 2. Key Features

- **Strict C Implementation**: Written 100% in standard C (C11), without C++, external runtime dependencies, or bloated GUI frameworks.
- **High-Resolution Wall-Clock Timer**: Employs hardware-backed performance counters (`QueryPerformanceCounter` on Windows, `clock_gettime(CLOCK_MONOTONIC)` on POSIX) achieving sub-microsecond timing accuracy.
- **Precise CPU Accounting**: Measures both **User Mode CPU Time** (time executing application code) and **Kernel/System CPU Time** (time executing OS system calls), alongside overall **CPU Utilization Percentage**.
- **Accurate Memory Profiling (Peak RSS)**: Tracks the physical memory resident in RAM (**Peak Working Set Size**) via Windows Process Status API (`GetProcessMemoryInfo` / `PSAPI`), pagefile commit charges, and page faults (soft and hard faults).
- **Process Tree & Child Aggregation**: Utilizes native **Windows Job Objects** (`CreateJobObject`, `AssignProcessToJobObject`, `JOBOBJECT_EXTENDED_LIMIT_INFORMATION`) to track and aggregate resource usage across an entire process tree, ensuring child processes spawned by shell scripts or compilers are fully accounted for.
- **I/O Counters**: Records exact byte counts and operation frequencies for file and device read/write calls via `GetProcessIoCounters`.
- **Configurable Timeout Enforcement**: Automatically terminates runaway or hanging processes after a user-defined threshold (`--timeout <time>`).
- **Benchmark / Repeat Mode**: Supports running benchmarks $N$ times (`--repeat <N>`) and calculating full descriptive statistics: minimum, maximum, sample mean, and standard deviation ($\sigma$).
- **Multi-Format Serialization**: Emits cleanly formatted ASCII tables, RFC 8259-compliant JSON (`--format json`), or RFC 4180-compliant CSV (`--format csv`).
- **Native Dual Interface**: Includes both a robust command-line interface (`crr.exe`) and an interactive graphical control panel (`crr_gui.exe`) built with pure Win32 API and common controls.

---

## 3. Architecture and Design

```
+-----------------------------------------------------------------------------------+
|                        COMMAND RESOURCE REPORTER (CRR)                            |
+-----------------------------------------------------------------------------------+
|   CLI Application (crr.exe)       |       GUI Control Panel (crr_gui.exe)         |
|   src/cli/main.c                  |       src/gui/gui_main.c                      |
+-----------------------------------+-----------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------------------+
|                          CORE ENGINE LIBRARY (libcrr.a)                           |
+-----------------------------------------------------------------------------------+
|  Process Supervisor & Timer       |  Data Formatters      |  Benchmark Statistics |
|  crr_executor_win.c / posix.c     |  crr_formatter.c      |  crr_stats.c          |
+-----------------------------------+-----------------------+-----------------------+
                                    |
        +---------------------------+---------------------------+
        |                                                       |
        v                                                       v
+--------------------------------+             +--------------------------------+
|     Windows NT Kernel API      |             |         POSIX Systems          |
| - CreateProcessA (CREATE_SUSP) |             | - fork() + execl(/bin/sh)      |
| - Windows Job Objects          |             | - wait4() / getrusage()        |
| - PSAPI / GetProcessMemoryInfo |             | - clock_gettime()              |
| - QueryPerformanceCounter      |             | - POSIX Pipes                  |
| - Win32 Anonymous Pipes        |             | - SIGKILL / Signals            |
+--------------------------------+             +--------------------------------+
```

---

## 4. Project Directory Layout

```
Command Resource Reporter/
├── Makefile                        # GNU Make build system
├── build.bat                       # 1-Click Windows build script
├── run_tests.bat                   # 1-Click test suite execution script
├── README.md                       # Complete evaluator documentation
├── REPORT.md                       # Comprehensive 10-section academic project report
├── include/
│   ├── crr.h                       # Public API types and metric structures
│   ├── crr_executor.h              # Execution engine interface & progress callbacks
│   ├── crr_formatter.h             # Human, JSON, and CSV serializers
│   ├── crr_platform.h              # OS platform abstraction layer
│   ├── crr_stats.h                 # Benchmark statistical computation engine
│   └── crr_version.h               # Version constants and metadata
├── src/
│   ├── cli/
│   │   └── main.c                  # Command line interface entry point
│   ├── core/
│   │   ├── crr_executor_win.c      # Windows Job Object & PSAPI supervisor
│   │   ├── crr_executor_posix.c    # POSIX rusage & wait4 supervisor
│   │   ├── crr_formatter.c         # Output formatting implementations
│   │   └── crr_stats.c             # Statistical variance/stddev algorithms
│   └── gui/
│       ├── gui_main.c              # Pure Win32 graphical dashboard in C
│       ├── resource.rc             # Windows resource file
│       └── app.manifest            # ComCtl32 v6 visual styles & DPI awareness
├── tests/
│   ├── test_runner.c               # Comprehensive automated test suite (66 asserts)
│   └── test_workloads/
│       ├── cpu_burn.c              # Controlled CPU load workload generator
│       ├── mem_burn.c              # Controlled physical memory commitment workload
│       ├── sleep_delay.c           # Controlled wall-clock delay workload
│       ├── child_spawner.c         # Multi-process spawner for tree verification
│       ├── fail_crash.c            # Fault injection & non-zero exit code workload
│       └── output_flood.c          # 200 KB stdout flood for pipe deadlock regression
├── tools/
│   ├── capture_gui.ps1             # Reproducible GUI screenshot capture script
│   └── test_gui_interactions.ps1   # Enter/resize/validation/cancel/close assertions against the real GUI
├── docs/
│   └── screenshots/                # GUI screenshots referenced by REPORT.md
├── .gitignore                      # Excludes bin/, build/, object files and test logs
└── bin/                            # Compiled binaries (generated on build)
```

---

## 5. Prerequisites

- **Operating System**: Windows 10/11 (or Linux/macOS for POSIX CLI).
- **C Compiler**: GCC (MinGW-w64 / MSYS2 UCRT64 / WinLibs) or Clang supporting C11.
- **Build Utilities**: `mingw32-make` (or GNU `make`) or standard Windows Command Prompt / PowerShell using `build.bat`.

---

## 6. Building the Project

### Option A: Using the Windows Batch Script (Recommended)
From PowerShell or Command Prompt in the project root:
```cmd
.\build.bat
```
This compiles the static library `libcrr.a`, CLI `bin\crr.exe`, GUI `bin\crr_gui.exe`, all 6 test workloads, and the test runner `bin\test_runner.exe`.

### Option B: Using GNU Make
```cmd
mingw32-make all
```
To clean build artifacts:
```cmd
mingw32-make clean
```

---

## 7. Running the CLI Application

### Basic Syntax
```cmd
bin\crr.exe [options] -- <command> [arguments...]
bin\crr.exe [options] "<command>"
```

### CLI Command Options
| Option | Argument | Description |
| :--- | :--- | :--- |
| `-h`, `--help` | None | Displays help message and exits |
| `-V`, `--version` | None | Displays version information |
| `-t`, `--timeout` | `<ms\|s\|m>` | Set execution timeout (e.g. `500ms`, `5s`, `2m`); invalid values exit 1 |
| `-n`, `--repeat` | `<count>` | Benchmark: repeat command $N$ times ($1 \le N \le 10000$) and report statistics |
| `-f`, `--format` | `human\|json\|csv` | Select output serialization format (default: `human`) |
| `-o`, `--output` | `<file>` | Write metrics report to file instead of stdout; write failures exit 1 |
| `-d`, `--dir` | `<directory>` | Set child process working directory |
| `-q`, `--quiet` | None | Suppress child process stdout/stderr |
| `-c`, `--capture` | None | Buffer child output and display before metrics; safe with `-n` |
| `--no-children` | None | Disable Windows Job Object tree tracking |
| `-v`, `--verbose` | None | Print diagnostics to stderr (parsed command, limits, per-run status/timing, CRR's own supervision CPU cost); stdout reports stay parseable |

### Practical Usage Examples

1. **Measure a network command:**
   ```cmd
   bin\crr.exe -- ping 127.0.0.1 -n 3
   ```

2. **Measure memory consumption with JSON output:**
   ```cmd
   bin\crr.exe -f json -o memory_report.json -- bin\mem_burn.exe 40
   ```

3. **Benchmark a compilation command 5 times with statistics:**
   ```cmd
   bin\crr.exe -n 5 -f human -- gcc -O2 -c src\core\crr_stats.c -o build\test.o
   ```

4. **Enforce a 500 millisecond timeout on a long task:**
   ```cmd
   bin\crr.exe -t 500ms -- bin\sleep_delay.exe 2000
   ```

5. **Generate CSV export for spreadsheet analysis:**
   ```cmd
   bin\crr.exe -f csv -o results.csv -- bin\cpu_burn.exe 300
   ```

---

## 8. Running the GUI Control Panel

Launch the native graphical control panel by running:
```cmd
bin\crr_gui.exe
```

### GUI Features & Controls
- **Command Input**: Type any arbitrary system command or executable path.
- **Quick Preset Buttons**: One-click test buttons (`CPU Burn`, `Mem Burn`, `Sleep Delay`, `Child Tree`) that insert the workload's absolute path (resolved next to `crr_gui.exe`), so they work regardless of the launch directory.
- **Working Directory Selector**: Includes text entry and a native folder picker dialog (`Browse...`).
- **Execution Settings**: Numeric fields for Timeout and Benchmark Repeat count, plus checkboxes for Job Object tree tracking and output capture.
- **Input Validation**: The timeout and repeat fields accept digits only (`0` = no timeout, repeat 1–10000). Letters, signs, or out-of-range values raise an `Invalid Input` message naming the field and return focus to it.
- **Worker Thread Execution**: Commands run on an asynchronous worker thread so the GUI remains 100% responsive with live timer and memory updates.
- **Real-Time Metrics Grid**: A 12-row dual-column ListView for a single run, switching to mean/min/max/std-dev rows for a benchmark.
- **Log / Output Console**: Monospaced console view displaying child stdout, stderr, and the generated report with proper line breaks.
- **Export Toolbar**: Native dialogs to export results to **JSON**, **CSV**, or copy the report directly to the Windows Clipboard.
- **Reproducible screenshots**: `tools\capture_gui.ps1` drives the real controls and captures the window (see `docs/screenshots/`).

Closing the window while a command is running cancels it, waits for the worker thread to exit, and only then releases state.

---

## 9. Testing and Validation

CRR includes an automated test runner (`bin\test_runner.exe`) containing **66 test assertions** across 15 test suites. The runner links the same `libcrr.a` used by the CLI and drives real child processes, so timings, exit codes, Job Object counts, and pipe behavior are genuine measurements rather than mocks. The process exits 0 when every assertion passes and 1 otherwise.

To run the complete test suite:
```cmd
.\run_tests.bat
```
or
```cmd
mingw32-make test
```

Expected last lines:
```
====================================================================
 TEST SUMMARY: Total: 66 | Passed: 66 | Failed: 0
====================================================================
```

### Verified Test Cases
1. **Options Initialization** (5): confirms zero-configuration defaults (no timeout, 1 repeat, tracking on, capture off).
2. **Byte & Time Formatting** (7): validates scale transitions (B, KiB, MiB, GiB) and duration formatting (ms, s, m).
3. **Statistical Math** (6): validates mean, min, max, and Bessel-corrected sample standard deviation against ground truth.
4. **JSON & CSV Serialization** (6): verifies valid schema generation, headers, and aggregate/per-run rows.
5. **Serializer Escaping** (8): verifies JSON escaping of Windows backslashes/quotes and RFC 4180 quote doubling in CSV.
6. **Error Handling & Validation** (6): NULL/empty commands, undersized buffers, unwritable output paths, and rejected CLI arguments.
7. **Exit Code Propagation** (2): verifies accurate reporting of both exit 0 and non-zero exit codes (e.g. exit 42).
8. **Wall-Clock Accuracy** (3): verifies timer duration under thread sleep workloads with near-zero CPU.
9. **CPU Time & Utilization** (2): verifies CPU burn load matches user CPU time and utilization calculations.
10. **Max RSS Measurement** (2): verifies that a 25 MiB commit produces $\ge 20\text{ MiB}$ peak working set and page faults.
11. **Timeout Enforcement** (3): verifies automatic termination of runaway processes upon reaching the timeout limit.
12. **Output Capture** (2): verifies complete stdout interception without loss.
13. **Pipe Drain Without Output Sink** (3): verifies that capture enabled with no output sink (the benchmark path) does not deadlock on a 200 KB stdout flood.
14. **Missing Command Classification** (6): verifies that an unresolvable name is a launch failure rather than a successful `cmd.exe` run that exits 1, while a real command exiting 42 and a shell built-in still succeed.
15. **Child Process Tree Tracking** (5): verifies multi-process Job Object accounting across parent and child tasks, including job-wide page-fault and I/O write totals.

### GUI Evidence
`tools\capture_gui.ps1` drives the real GUI (preset → Run → wait for completion) and saves PNGs via `PrintWindow`:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\capture_gui.ps1
```
The resulting images are referenced by [REPORT.md](./REPORT.md) §6.1.

Interactive behavior is asserted separately by `tools\test_gui_interactions.ps1` (19 checks), which presses Enter in the command field, resizes the window and verifies every control reflows and returns to its original rectangle, rejects invalid timeout/repeat values with a field-specific message, cancels a 6 s run, and closes the window mid-run:
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\test_gui_interactions.ps1
```
Expected output ends with `GUI interaction failures: 0` and exit code 0.

---

## 10. Sample Output

### Human-Readable CLI Report
```
================================================================================
 COMMAND RESOURCE USAGE REPORT (1.0.0)
================================================================================
 Command Line         : bin/cpu_burn.exe 250
 Working Directory    : <current>
 Status               : SUCCESS (Exit Code: 0)
 Child Processes      : 1 tracked [Job Object tree active]
--------------------------------------------------------------------------------
 TIME METRICS
   Wall-Clock Elapsed :     0.360670 s  (360.67 ms)
   User CPU Time      :     0.234375 s  (234.38 ms)
   Kernel/System CPU  :     0.000000 s  (0.00 ms)
   Total CPU Time     :     0.234375 s  (234.38 ms)
   CPU Utilization    :        64.98 %
--------------------------------------------------------------------------------
 MEMORY METRICS
   Max Resident Set   :      4354048 B  (4.15 MiB)
   Peak Commit Charge :       671744 B  (656.00 KiB)
   Page Faults        :         1106
--------------------------------------------------------------------------------
 I/O METRICS
   File / Device Read :         1826 B  (1.78 KiB, 1 ops)
   File / Device Write:           56 B  (56 B, 1 ops)
================================================================================
```
Measured with `bin\crr.exe -- bin\cpu_burn.exe 250` on Windows 11 (`10.0.26300.9818`) with GCC 15.2.0. Values vary by run; the JSON/CSV appendices in [REPORT.md](./REPORT.md) §10 contain further real samples.

---

## 11. Known Limitations

- **POSIX fallback is reduced**: timeouts terminate only the direct `/bin/sh` child (no process group), I/O byte counters are not populated (`getrusage` block counts land in the operation fields), commit charge is 0, progress callbacks always report RSS 0, and a shell exit 127 is not reclassified as a launch failure.
- **GUI**: Windows-only, and the layout scales proportionally rather than reflowing: controls keep their design-time positions, stretched with the window, so a very wide window makes the two panes taller/wider but not better balanced. Minimum size is 860×620. Fonts are not re-scaled for high-DPI displays. Pressing Enter in the command field does trigger Run — the field is subclassed and routes the key through the same enabled/disabled Run path as the button.
- **ANSI code page**: paths/commands using characters outside the active code page may be misrendered (the `A` Win32 APIs are used throughout).
- **Observer overhead is quantified, not zero**: `-v` reports CRR's own CPU cost while supervising. It is a fixed per-run charge of about one scheduler tick, which is 0.04% of a 12-core host for a 3 s target but dominates sub-second runs (see REPORT.md §7.6). Windows quantizes process CPU counters to 15.625 ms, so sub-tick costs are reported as `< 15.6 ms`.
- **Privilege separation**: processes spawned with elevated UAC privileges from an un-elevated prompt cannot be assigned to user-level Job Objects due to Windows kernel security restrictions.
- **Short-lived micro-tasks**: for processes running under 1 millisecond, standard OS scheduling quantum (15.6 ms timer interrupt on Windows) reports quantized CPU time increments. High-precision wall-clock time remains accurate to $< 1\,\mu\text{s}$.
