/**
 * @file mem_burn.c
 * @brief Controlled memory allocation and page-commit test workload.
 *
 * Systems Testing Purpose:
 * When a process calls malloc(), modern virtual-memory operating systems perform
 * "optimistic allocation" (lazy paging) — pages are mapped into virtual address space
 * but are NOT committed to physical RAM until touched.
 *
 * This workload:
 * 1. Allocates a specified quantity of MiB.
 * 2. Writes to every 4096-byte memory page (touching pages).
 * 3. Forces the OS kernel to fault in real physical memory frames, expanding
 *    the process's Working Set (Resident Set Size).
 * 4. Verifies that CRR records the resulting Peak RSS and PageFaultCount accurately.
 *
 * Usage:
 *   mem_burn.exe [mib_to_allocate]   (default: 30 MiB)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#define SLEEP_MS(x) Sleep(x)
#else
#include <unistd.h>
#define SLEEP_MS(x) usleep((x)*1000)
#endif

int main(int argc, char *argv[]) {
    long mib = 30;
    if (argc > 1) {
        mib = strtol(argv[1], NULL, 10);
        if (mib <= 0) mib = 30;
    }

    size_t bytes = (size_t)mib * 1024ULL * 1024ULL;
    char *buf = (char*)malloc(bytes);
    if (!buf) {
        fprintf(stderr, "Failed to allocate %ld MiB\n", mib);
        return 1;
    }

    /* Touch every 4 KiB virtual page to force physical memory commit (Resident Set) */
    size_t page_size = 4096;
    for (size_t i = 0; i < bytes; i += page_size) {
        buf[i] = (char)(i & 0xFF);
    }
    buf[bytes - 1] = 42;

    printf("mem_burn committed %ld MiB (%zu bytes)\n", mib, bytes);
    fflush(stdout);

    /* Hold memory allocation briefly so OS metrics sampler captures the peak */
    SLEEP_MS(80);

    free(buf);
    return 0;
}
