/**
 * @file child_spawner.c
 * @brief Multi-process workload spawner for process tree verification.
 *
 * Systems Testing Purpose:
 * Spawns multiple independent child processes (cpu_burn and mem_burn).
 * Used to verify that:
 * 1. Windows Job Objects track the entire process tree automatically.
 * 2. Child process counts (> 1) are accurately captured.
 * 3. Child CPU times and child memory footprints are aggregated into the parent's metrics report.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#include <errno.h>

static int run_sibling_workload(const char *name, const char *argument) {
    char executable[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, executable, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        fprintf(stderr, "child_spawner: could not locate its executable path\n");
        return 1;
    }

    char *separator = strrchr(executable, '\\');
    char *forward_separator = strrchr(executable, '/');
    if (!separator || (forward_separator && forward_separator > separator)) {
        separator = forward_separator;
    }
    if (!separator) {
        fprintf(stderr, "child_spawner: executable path has no parent directory\n");
        return 1;
    }

    size_t directory_length = (size_t)(separator + 1 - executable);
    if (directory_length + strlen(name) >= sizeof(executable)) {
        fprintf(stderr, "child_spawner: workload path is too long\n");
        return 1;
    }
    strcpy(executable + directory_length, name);

    intptr_t exit_code = _spawnl(_P_WAIT, executable, executable, argument, NULL);
    if (exit_code == -1) {
        fprintf(stderr, "child_spawner: could not launch %s (errno %d)\n", executable, errno);
        return 1;
    }
    if (exit_code != 0) {
        fprintf(stderr, "child_spawner: %s exited with code %ld\n", name, (long)exit_code);
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("child_spawner: Launching child tasks...\n");
    fflush(stdout);

    int failed = 0;
    failed |= run_sibling_workload("cpu_burn.exe", "150");
    failed |= run_sibling_workload("mem_burn.exe", "20");

    if (failed) {
        return 1;
    }
    printf("child_spawner: All child tasks completed.\n");
    return 0;
}
#else
#include <unistd.h>
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("child_spawner: Launching child tasks...\n");

    /* Spawn Child 1 */
    system("bin/cpu_burn 150");

    /* Spawn Child 2 */
    system("bin/mem_burn 20");

    printf("child_spawner: All child tasks completed.\n");
    return 0;
}
#endif
