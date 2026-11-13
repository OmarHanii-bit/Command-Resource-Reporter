@echo off
setlocal enabledelayedexpansion

echo ====================================================================
echo  BUILDING COMMAND RESOURCE REPORTER (CRR) - 100%% C IMPLEMENTATION
echo ====================================================================

if not exist bin mkdir bin
if not exist build mkdir build

set CFLAGS=-std=c11 -Wall -Wextra -O2 -Iinclude

echo [1/8] Compiling Core Objects...
gcc %CFLAGS% -c src/core/crr_executor_win.c -o build/crr_executor_win.o
if %ERRORLEVEL% NEQ 0 ( echo Failed to compile crr_executor_win.c & exit /b 1 )

gcc %CFLAGS% -c src/core/crr_formatter.c -o build/crr_formatter.o
if %ERRORLEVEL% NEQ 0 ( echo Failed to compile crr_formatter.c & exit /b 1 )

gcc %CFLAGS% -c src/core/crr_stats.c -o build/crr_stats.o
if %ERRORLEVEL% NEQ 0 ( echo Failed to compile crr_stats.c & exit /b 1 )

echo [2/8] Creating Static Library build/libcrr.a...
ar rcs build/libcrr.a build/crr_executor_win.o build/crr_formatter.o build/crr_stats.o
if %ERRORLEVEL% NEQ 0 ( echo Failed to create static library & exit /b 1 )

echo [3/8] Building CLI Executable (bin/crr.exe)...
gcc %CFLAGS% src/cli/main.c build/libcrr.a -lpsapi -o bin/crr.exe
if %ERRORLEVEL% NEQ 0 ( echo Failed to build CLI executable & exit /b 1 )

echo [4/8] Compiling Windows Manifest and Resources...
windres -i src/gui/resource.rc -o build/resource.o
if %ERRORLEVEL% NEQ 0 ( echo Failed to compile resources & exit /b 1 )

echo [5/8] Building GUI Executable (bin/crr_gui.exe)...
gcc %CFLAGS% src/gui/gui_main.c build/resource.o build/libcrr.a -mwindows -lcomctl32 -lcomdlg32 -lshlwapi -lole32 -lpsapi -o bin/crr_gui.exe
if %ERRORLEVEL% NEQ 0 ( echo Failed to build GUI executable & exit /b 1 )

echo [6/8] Building Test Workloads...
gcc %CFLAGS% tests/test_workloads/cpu_burn.c -o bin/cpu_burn.exe
gcc %CFLAGS% tests/test_workloads/mem_burn.c -o bin/mem_burn.exe
gcc %CFLAGS% tests/test_workloads/sleep_delay.c -o bin/sleep_delay.exe
gcc %CFLAGS% tests/test_workloads/child_spawner.c -o bin/child_spawner.exe
gcc %CFLAGS% tests/test_workloads/fail_crash.c -o bin/fail_crash.exe
gcc %CFLAGS% tests/test_workloads/output_flood.c -o bin/output_flood.exe
gcc %CFLAGS% tests/test_workloads/orphan_spawner.c -o bin/orphan_spawner.exe

echo [7/8] Building Comprehensive Test Runner (bin/test_runner.exe)...
gcc %CFLAGS% tests/test_runner.c build/libcrr.a -lpsapi -o bin/test_runner.exe
if %ERRORLEVEL% NEQ 0 ( echo Failed to build test runner & exit /b 1 )

echo [8/8] Build Complete!
echo Binaries generated:
echo   - CLI Application : bin\crr.exe
echo   - GUI Application : bin\crr_gui.exe
echo   - Test Runner     : bin\test_runner.exe
echo   - Test Workloads  : bin\*_burn.exe, bin\*_delay.exe, etc.
echo ====================================================================
