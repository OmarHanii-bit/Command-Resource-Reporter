/**
 * @file cpu_burn.c
 * @brief Controlled CPU-intensive test workload.
 *
 * Systems Testing Purpose:
 * Generates an active, high-intensity arithmetic computational loop for a designated
 * duration in milliseconds. Used to verify that CRR accurately measures:
 * 1. User Mode CPU time (which should reflect active core execution).
 * 2. High CPU utilization percentage (typically > 80% on the executing core).
 *
 * Usage:
 *   cpu_burn.exe [duration_ms]   (default: 200 ms)
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
/**
 * @brief Returns high-resolution elapsed time in seconds on Windows.
 */
static double get_clock_sec(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / (double)freq.QuadPart;
}
#else
#include <time.h>
/**
 * @brief Returns monotonic elapsed time in seconds on POSIX.
 */
static double get_clock_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
#endif

int main(int argc, char *argv[]) {
    long target_ms = 200;
    if (argc > 1) {
        target_ms = strtol(argv[1], NULL, 10);
        if (target_ms <= 0) target_ms = 200;
    }

    double start = get_clock_sec();
    double target_sec = (double)target_ms / 1000.0;
    /* Declare dummy as volatile to prevent optimizing compiler from eliding the loop */
    volatile double dummy = 1.0;

    /* Execute computational loop until target duration expires */
    while ((get_clock_sec() - start) < target_sec) {
        for (int i = 0; i < 10000; ++i) {
            dummy = dummy * 1.0000001 + 0.0000001;
        }
    }

    printf("cpu_burn completed (target: %ld ms, dummy=%f)\n", target_ms, dummy);
    return 0;
}
