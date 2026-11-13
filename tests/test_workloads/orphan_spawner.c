/**
 * @file orphan_spawner.c
 * @brief Starts a descendant that outlives the root process while holding stdout.
 *
 * Regression workload: the root exits immediately, but the detached child keeps
 * the inherited pipe write end open for several seconds. A supervisor that
 * blocks on ReadFile until EOF therefore hangs well past the root's exit.
 */

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

int main(void) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);

    char child_cmd[128];
    snprintf(child_cmd, sizeof(child_cmd), "cmd.exe /c ping -n 9 127.0.0.1 >nul");

    if (!CreateProcessA(NULL, child_cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "orphan_spawner: could not launch the detached child\n");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    printf("orphan_spawner: detached child started\n");
    fflush(stdout);
    return 0;
}
#else
#include <unistd.h>

int main(void) {
    printf("orphan_spawner: detached child started\n");
    fflush(stdout);
    if (fork() == 0) {
        execlp("sleep", "sleep", "8", (char*)NULL);
        _exit(0);
    }
    return 0;
}
#endif
