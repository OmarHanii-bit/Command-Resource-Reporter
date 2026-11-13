# Change Summary — Final Submission

**Project:** Command Resource Reporter (CRR)
**Course deliverable:** Project Report, Project Implementation, Graphical User Interface
**Prepared:** 2026-11-11
**Baseline:** commit `aaa692a` ("Validate GUI timeout and repeat fields")
**Final state:** the changes described in §1–§3 are in the working tree at packaging time and are included in the submission archive.

This document records what changed after the initial implementation, why each change was made,
and the evidence that it works. The academic report ([REPORT.md](./REPORT.md)) carries the full
analysis; this is the consolidated change log.

---

## 1. Final code-review round

A full review of the final codebase produced four defect groups. Each was reproduced before being
fixed: the GUI driver and test runner were extended first, the new checks were watched failing
(22-check GUI run: 2 failures; 70-assertion unit run: 1 failure plus the new drain regression), and
only then was the production code changed.

### 1.1 A report could hang for as long as a detached child lived

**Symptom:** a command that starts a background child and exits immediately produced no report until
that child terminated. `orphan_spawner.exe` (new workload) returns at once while its detached child
holds the inherited stdout pipe for about 8 seconds; the supervisor took ~8 s to finish.

**Root cause:** the post-exit drain called blocking `ReadFile` until EOF. EOF only arrives when every
process holding the pipe's write end closes it, including detached descendants that are not part of
the command's own result.

**Fix:** [`drain_pipe_eof()`](./src/core/crr_executor_win.c) now polls `PeekNamedPipe`, consumes the
bytes the root left buffered, and stops 250 ms after the last byte. Normal runs still end immediately
(the write end is already closed, so the first peek fails); only a surviving writer is cut short.
The truncation behaviour is documented as a limitation in REPORT.md §7.5 and README §11.

**Evidence:** new workload [`orphan_spawner.c`](./tests/test_workloads/orphan_spawner.c) plus two
assertions in the "Detached Descendant Holding stdout" suite. Before the fix the report returned
after ~8 s; now it returns in ~0.3 s, and the assertion enforces < 2.5 s.

### 1.2 The GUI could export mid-run, and a cancelled benchmark counted the interrupted run

**Symptom:** Export JSON/CSV and Copy Report stayed clickable during a benchmark, so a file dialog
could open over a half-updated report. Cancelling a 3-run benchmark reported
"cancelled after 1 of 3 runs" although zero runs had completed, and the summary statistics included
the aborted run.

**Root cause:** the Run handler disabled only Run/Cancel, and the benchmark loop incremented
`multi_runs_count` immediately after `crr_execute_with_callbacks` returned, before checking whether
that run was cancelled.

**Fix:** one helper in [`src/gui/gui_main.c`](./src/gui/gui_main.c) now owns the enabled state of Run,
Cancel, Export JSON, Export CSV, and Copy Report; it runs on start, completion, and thread-creation
failure. The export handlers also refuse to act while `is_running`. The benchmark loop breaks on
`CRR_ERR_CANCELLED` before the count is advanced, so statistics cover completed runs only.

**Evidence:** three new checks in [`tools/test_gui_interactions.ps1`](./tools/test_gui_interactions.ps1):
exports disabled while running, exports restored afterwards, and "Benchmark cancelled after 0 of 3 runs."
The script now also resets the status label through the Clear button before each wait, removing a
false pass where a completion message from an earlier step satisfied the next wait.

### 1.3 Window-close and clipboard races

**Symptom:** closing the window immediately after pressing Run could free buffers while the worker was
still starting. A failed clipboard copy could dereference a failed `GlobalLock` or leak the allocation.

**Root cause:** `WM_DESTROY` waited only when `is_running` was already true, but the worker sets that
flag after `CreateThread` returns, so a close in that gap skipped the wait. The teardown then freed
state even if the wait timed out. The clipboard path passed `GlobalLock(hGlob)` straight to `memcpy`
and ignored the result of `SetClipboardData`.

**Fix:** teardown now waits whenever a thread handle exists, cancels unconditionally, frees state only
after `WAIT_OBJECT_0`, and otherwise leaves the buffers alone for process exit. Clipboard copy checks
the lock, frees the handle when `SetClipboardData` fails, and shows an error dialog instead of
reporting success.

**Evidence:** the existing close-mid-run check still passes (process exits in ~0.15 s, exit code 0).

### 1.4 Executor and CLI hardening

| Issue | Fix |
| :--- | :--- |
| `WAIT_FAILED` fell through to the blocking EOF drain while the child was still alive | Terminate the process/job on `WAIT_FAILED` and skip the EOF drain |
| The first pipe handle leaked when the second `CreatePipe` failed | Close every pipe handle on creation failure |
| `AssignProcessToJobObject` result was ignored, leaving queries against a job that tracked nothing | Close and drop the job handle when assignment fails |
| Job-wide `PeakJobMemoryUsed` (peak commit) was folded into `max_rss_bytes` (peak working set) | It now folds into `peak_pagefile_bytes`; the child-tree assertion checks the tree-wide commit value |
| `strtod("nan")` passed every range check and was cast to `uint32_t` | Reject non-finite values with `isfinite()` before conversion |
| `-t nan` and a missing CLI binary were untested | Two new assertions in the error-handling suite |

---

## 2. Course-rubric fixes

### 2.1 GUI input validation

The GUI timeout and repeat fields were read with `strtoul` without end-pointer checks:
`abc` silently meant "no timeout", `-5` wrapped to a huge unsigned value, and `seconds * 1000`
could overflow. [`parse_uint_field()`](./src/gui/gui_main.c) now accepts only optional spaces and
decimal digits, treats empty timeout as 0, and range-checks timeout (≤ 4,294,967 s) and repeat
(1–10,000). Invalid input opens an `Invalid Input` message that names the field and refocuses it.

**Evidence:** eight GUI checks cover `abc`/`-5` on timeout and `abc`/`0` on repeat, the message text,
and a successful run with valid values.

### 2.2 Portable quick presets

Preset buttons used relative paths resolved against the process working directory, so they failed
when `crr_gui.exe` was started from another folder. `set_preset_command()` builds an absolute quoted
path from `GetModuleFileNameA`'s directory. Screenshot 2 in REPORT.md §6.1 shows the resolved path.

---

## 3. GUI resizing and console-host fix

- The window is resizable with a minimum client size of 860×620; `WM_SIZE` rescales every child from
  its captured design rectangle, and `WM_GETMINMAXINFO` enforces the minimum. Fonts stay fixed,
  which is recorded as a limitation.
- A GUI-launched console child could fail to allocate a console (`ERROR_NO_SYSTEM_RESOURCES`,
  `0x800700E8`) because the GUI owns no console and the default terminal host refused the request.
  When output is redirected (`-c`/`-q`) and `GetConsoleWindow()` is `NULL`, children are now spawned
  with `CREATE_NO_WINDOW`.

---

## 4. Documentation and evidence synchronization

- `REPORT.md`: §2 architecture diagram, §6.1 screenshots, §7.2–§7.5 test evidence and defect table,
  §8.2 portable preset path, §8.3 expected test output, and the appendix transcripts now match the
  code. Test counts are **70 assertions / 16 suites** and **22 GUI checks**.
- `REPORT_drawio.md`: regenerated from `REPORT.md`; the only difference is §4.1, which points at
  [`REPORT_architecture.drawio`](./REPORT_architecture.drawio).
- `README.md`: badge and tree counts, verified-test list (including the new drain suite), GUI driver
  description, and the detached-descendant limitation.
- `FILE_LINKS.md`: documents the new `orphan_spawner.c` workload and the expanded GUI driver.
- `tools/capture_gui.ps1` regenerates the screenshots referenced by REPORT.md §6.1.

---

## 5. Verification evidence

All commands are run from the repository root on Windows 11 x64 with GCC 15.2.0 (UCRT64).

```cmd
build.bat
bin\test_runner.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tools\test_gui_interactions.ps1
```

| Check | Result |
| :--- | :--- |
| Build (`build.bat`, 8 stages) | Clean, no warnings |
| Unit / integration runner | **Total: 70, Passed: 70, Failed: 0**, exit code 0 |
| GUI interaction driver | **22 checks, 0 failures**, exit code 0 |
| Repeat runs | Both suites re-run; identical results |
| Manual spot checks | `-t nan` rejected; detached-child report returns in ~0.3 s |

---

## 6. Submission archive

The archive contains the full source tree, documentation, tests, tooling, and the prebuilt `bin\`
executables. It excludes `.git\`, `build\` intermediates, `tests_out.txt`, logs, and the archive
itself. To verify the archive, extract it to a fresh directory and run `build.bat`, then the two
test commands above; both must end with the results in §5.

Known limitations are listed in REPORT.md §7.5 and README §11. They are recorded, not hidden: the
POSIX fallback is reduced, the GUI is Windows-only with DPI-unscaled fonts, ANSI APIs limit
non-ASCII paths, export dialogs are modal and untested by automation, and output written by a
detached descendant after the root exits is not included in the report.
