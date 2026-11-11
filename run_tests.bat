@echo off
echo ====================================================================
echo  RUNNING COMMAND RESOURCE REPORTER TEST SUITE
echo ====================================================================

if not exist bin\test_runner.exe (
    echo Building project before running tests...
    call build.bat
    if %ERRORLEVEL% NEQ 0 exit /b 1
)

bin\test_runner.exe
