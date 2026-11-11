/**
 * @file crr_platform.h
 * @brief Platform abstraction and operating system detection header.
 *
 * This file detects whether the compilation target is Microsoft Windows or a
 * POSIX-compliant operating system (Linux, macOS, BSD). It pulls in the appropriate
 * operating system kernel headers, ensuring clean separation of concerns and preventing
 * header pollution.
 */

#ifndef CRR_PLATFORM_H
#define CRR_PLATFORM_H

#if defined(_WIN32) || defined(_WIN64)
    /* Microsoft Windows target */
    #define CRR_PLATFORM_WINDOWS 1

    /* Exclude rarely-used services from Windows headers to accelerate compilation */
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif

    #include <windows.h>  /* Core Win32 APIs: processes, threads, timers, job objects */
    #include <psapi.h>    /* Process Status API: GetProcessMemoryInfo for Working Set */

#else
    /* POSIX-compliant target (Linux, macOS, BSD, UNIX) */
    #define CRR_PLATFORM_POSIX 1

    #include <sys/types.h>     /* Primitive system data types (pid_t, etc.) */
    #include <sys/wait.h>      /* Process wait functions: waitpid, wait4, WIFEXITED */
    #include <sys/time.h>      /* High-resolution timeval and interval timers */
    #include <sys/resource.h>  /* Process resource operations: getrusage, rusage struct */
    #include <unistd.h>        /* Standard symbolic constants: fork, exec, pipe, dup2 */
    #include <signal.h>        /* Signal handling: kill, SIGKILL, SIGTERM */
    #include <time.h>          /* POSIX monotonic clocks: clock_gettime */
#endif

#endif /* CRR_PLATFORM_H */
