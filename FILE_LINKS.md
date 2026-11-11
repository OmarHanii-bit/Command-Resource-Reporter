# CRR File Relationships

This guide explains how the project files connect. The authoritative report is
[`REPORT.md`](./REPORT.md) (its draw.io-diagram twin is
[`REPORT_drawio.md`](./REPORT_drawio.md)). For a visual overview, open the
editable, two-page diagram
[`PROJECT_FILE_MAP.drawio`](./PROJECT_FILE_MAP.drawio) in diagrams.net / draw.io.

## 1. Shared headers and core implementation

- [`include/crr.h`](./include/crr.h) defines the common options, result metrics,
  output buffers, status codes, and formats used across the program.
- [`include/crr_executor.h`](./include/crr_executor.h) declares the process
  execution API and includes `crr.h`.
- [`include/crr_platform.h`](./include/crr_platform.h) selects the Windows or
  POSIX operating-system headers for the corresponding executor.
- [`include/crr_stats.h`](./include/crr_stats.h) declares the benchmark
  statistics types and functions; it uses the shared types from `crr.h`.
- [`include/crr_formatter.h`](./include/crr_formatter.h) declares human,
  JSON, and CSV formatting functions and uses the metrics/statistics types.
- [`include/crr_version.h`](./include/crr_version.h) holds version metadata
  used in reports and application output.

The matching core implementations are:

- [`src/core/crr_executor_win.c`](./src/core/crr_executor_win.c) implements
  process execution and resource collection with Windows APIs.
- [`src/core/crr_executor_posix.c`](./src/core/crr_executor_posix.c) provides
  the POSIX executor. The build selects this **or** the Windows executor,
  depending on the target platform.
- [`src/core/crr_stats.c`](./src/core/crr_stats.c) computes repeated-run
  statistics using the types declared in `crr_stats.h`.
- [`src/core/crr_formatter.c`](./src/core/crr_formatter.c) converts measured
  metrics into human-readable, JSON, or CSV reports.

## 2. Applications use the core

- [`src/cli/main.c`](./src/cli/main.c) parses command-line options, calls the
  executor, optionally computes benchmark statistics, and sends results to a
  formatter.
- [`src/gui/gui_main.c`](./src/gui/gui_main.c) provides the Windows interface.
  Its worker thread calls the executor; the GUI uses the statistics and
  formatter APIs to display or export results.
- [`src/gui/resource.rc`](./src/gui/resource.rc) embeds
  [`src/gui/app.manifest`](./src/gui/app.manifest) into the Windows GUI build.

Both applications link against the compiled core library; the GUI also links
the compiled Windows resource.

## 3. Tests and workload programs

- [`tests/test_runner.c`](./tests/test_runner.c) tests core behavior and runs
  the workload executables. It links against the core library.
- The standalone workload sources in
  [`tests/test_workloads/`](./tests/test_workloads) provide controlled CPU,
  memory, sleep, child-process, exit-code, and pipe-flood test cases.
- [`tests/test_workloads/child_spawner.c`](./tests/test_workloads/child_spawner.c)
  launches the CPU and memory workloads beside itself to exercise process-tree
  tracking.
- [`tests/test_workloads/output_flood.c`](./tests/test_workloads/output_flood.c)
  writes 200 KB to stdout so the runner can prove that benchmark mode drains
  pipes without hanging.

## 4. Report evidence and screenshot tooling

- [`REPORT.md`](./REPORT.md) contains the full report; [`REPORT_drawio.md`](./REPORT_drawio.md)
  is the same text with §4.1 pointing at [`REPORT_architecture.drawio`](./REPORT_architecture.drawio).
- [`tools/capture_gui.ps1`](./tools/capture_gui.ps1) launches `bin/crr_gui.exe`,
  clicks the real preset/Run buttons, waits for the worker thread, captures the
  window with `PrintWindow`, and closes it. It writes the PNGs in
  [`docs/screenshots/`](./docs/screenshots) that §6.1 of the report embeds.
- [`tools/test_gui_interactions.ps1`](./tools/test_gui_interactions.ps1) drives
  Enter-to-run, proportional resize/reflow, invalid-field rejection, Cancel
  during a run, and closing the window mid-run, asserting the status text,
  dialog messages, and fast teardown. It exits 0 only when every check passes.

## 5. Build and test flow

- [`Makefile`](./Makefile) selects the platform executor, compiles the core
  objects, archives the library, and builds the applications and tests.
- [`build.bat`](./build.bat) is the Windows build script. It compiles the
  Windows executor, formatter, and statistics modules into
  `build/libcrr.a`, then links the CLI, GUI, test runner, and workloads under
  `bin/`.
- [`run_tests.bat`](./run_tests.bat) runs the Windows test runner, building the
  project first if that executable is missing.

### Relationship shorthand

```text
headers -> core source files -> static library -> CLI / GUI / test runner
GUI resource script + manifest -----------------> GUI executable
workload source files --------------------------> workload executables
test runner + static library + workloads -------> automated tests
```

The arrows describe source, build, and runtime relationships; they do not mean
that every source file directly includes every header shown in the diagram.
