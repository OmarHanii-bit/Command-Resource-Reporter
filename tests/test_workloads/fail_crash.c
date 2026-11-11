/**
 * @file fail_crash.c
 * @brief Fault injection and exit code verification workload.
 *
 * Systems Testing Purpose:
 * Returns a specific non-zero exit code or deliberately triggers an unhandled
 * memory exception (SIGSEGV / EXCEPTION_ACCESS_VIOLATION).
 * Used to verify:
 * 1. Accurate propagation and display of non-zero process exit codes (e.g. exit code 42).
 * 2. Proper error state handling without supervisor crash or lockup.
 *
 * Usage:
 *   fail_crash.exe [code|"crash"]   (default: exit code 42)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    int code = 42;
    if (argc > 1) {
        if (strcmp(argv[1], "crash") == 0) {
            printf("fail_crash: Intentionally triggering crash...\n");
            fflush(stdout);
            /* Deliberate null-pointer dereference to test crash handling */
            volatile int *ptr = NULL;
            *ptr = 123;
        }
        code = atoi(argv[1]);
    }
    printf("fail_crash: Exiting with code %d\n", code);
    return code;
}
