/**
 * @file sleep_delay.c
 * @brief Controlled wall-clock delay test workload.
 *
 * Systems Testing Purpose:
 * Puts the calling thread into a sleeping state for a designated number of milliseconds.
 * Used to verify:
 * 1. Wall-clock elapsed time matches the requested sleep duration.
 * 2. User CPU time and Kernel CPU time remain near zero (< 15 ms).
 * 3. Overall CPU utilization percentage remains minimal (< 5%).
 * 4. Timeout watchdog triggers accurately when sleep exceeds the configured threshold.
 *
 * Usage:
 *   sleep_delay.exe [duration_ms]   (default: 300 ms)
 */

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x)*1000)
#endif

int main(int argc, char *argv[]) {
    long ms = 300;
    if (argc > 1) {
        ms = strtol(argv[1], NULL, 10);
        if (ms <= 0) ms = 300;
    }

    printf("sleep_delay sleeping for %ld ms...\n", ms);
    fflush(stdout);

    /* Suspend thread execution via OS scheduler */
    SLEEP_MS(ms);

    printf("sleep_delay finished.\n");
    return 0;
}
