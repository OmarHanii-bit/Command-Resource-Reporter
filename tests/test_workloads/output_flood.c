/**
 * @file output_flood.c
 * @brief High-volume output workload for pipe-drain and deadlock verification.
 *
 * Systems Testing Purpose:
 * Writes a configurable number of bytes to stdout and a short message to stderr.
 * Used to verify that the supervisor keeps draining redirected pipes even when the
 * caller does not request capture. If the parent stops reading while the pipe
 * buffer fills, the child blocks and the execution never completes.
 *
 * Usage:
 *   output_flood.exe [bytes]   (default: 200000 bytes)
 */

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    long target = 200000;
    if (argc > 1) {
        target = strtol(argv[1], NULL, 10);
        if (target <= 0) target = 200000;
    }

    char chunk[4096];
    for (size_t i = 0; i < sizeof(chunk); ++i) {
        chunk[i] = 'x';
    }

    long written = 0;
    while (written < target) {
        long remaining = target - written;
        size_t to_write = (remaining > (long)sizeof(chunk)) ? sizeof(chunk) : (size_t)remaining;
        if (fwrite(chunk, 1, to_write, stdout) != to_write) {
            fprintf(stderr, "output_flood: stdout write failed after %ld bytes\n", written);
            return 1;
        }
        written += (long)to_write;
    }
    fputc('\n', stdout);

    fprintf(stderr, "output_flood: stderr channel active after %ld bytes\n", written);
    fflush(stdout);
    fflush(stderr);
    return 0;
}
