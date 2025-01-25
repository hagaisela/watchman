/**
 * watchman_test.c
 *
 * Demonstrates how to do an in-process backtrace when we receive SIGUSR2
 * from the "watchman" external debugger. We also run a thread that modifies
 * some global variables indefinitely.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <string.h>

// We'll use libbacktrace in-process
#include <backtrace.h>
#include <backtrace-supported.h>

// Global variables that the debugger will watch
volatile int   g_varA = 100;
volatile long  g_varB = 200;
volatile char  g_varC = 1;

// We'll store a global backtrace_state*
static struct backtrace_state *g_state = NULL;

// Error callback for libbacktrace
static void error_callback(void *data, const char *msg, int errnum) {
    (void)data; // unused
    fprintf(stderr, "[watchman_test] backtrace error: %s (err=%d)\n", msg, errnum);
}

// Callback for each frame in backtrace
static int full_callback(void *data, uintptr_t pc,
                         const char *filename, int lineno,
                         const char *function) {
    (void)data; // unused
    if (function && filename) {
        fprintf(stderr, "  => %s:%d: %s()\n", filename, lineno, function);
    } else if (filename) {
        fprintf(stderr, "  => %s:%d\n", filename, lineno);
    } else {
        fprintf(stderr, "  => [0x%lx]\n", (unsigned long)pc);
    }
    // Returning 0 => keep going. If you only wanted top frames, could return 1 to stop.
    return 0;
}

// Perform a full in-process backtrace
static void do_local_backtrace(void) {
    // The simplest approach with libbacktrace is backtrace_full:
    //  - skip=0 to decode from this function onward
    backtrace_full(g_state, /*skip=*/0, full_callback, error_callback, NULL);
}

// SIGUSR2 handler => show local backtrace
static void sigusr2_handler(int sig, siginfo_t *info, void *ucontext) {
    (void)sig;
    (void)info;
    (void)ucontext;

    fprintf(stderr, "[watchman_test] Caught SIGUSR2 => hardware watchpoint triggered?\n");
    fprintf(stderr, "[watchman_test] Dumping local backtrace:\n");
    do_local_backtrace();
    fprintf(stderr, "[watchman_test] End of local backtrace. Continuing.\n");
}

// Worker thread => modifies g_varA, g_varB, g_varC in a loop
static void* thread_func(void *arg) {
    (void)arg;
    fprintf(stderr, "[watchman_test] Thread started.\n");
    for (int i = 0; ; i++) {
        g_varA += 2;
        g_varB += 10;
        g_varC++;
        fprintf(stderr, "[thread] iteration=%d => g_varA=%d, g_varB=%ld, g_varC=%d\n",
                i, g_varA, g_varB, g_varC);
        usleep(300000); // 300 ms
    }
    return NULL; // unreachable
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    fprintf(stderr, "[watchman_test] Starting main\n");
    pid_t pid = getpid();
    fprintf(stderr, "[watchman_test] pid=%d\n", (int)pid);

    // Print addresses (for the debugger)
    fprintf(stderr, "  g_varA=%p\n", (void*)&g_varA);
    fprintf(stderr, "  g_varB=%p\n", (void*)&g_varB);
    fprintf(stderr, "  g_varC=%p\n", (void*)&g_varC);

    // 1) Initialize libbacktrace for in-process backtraces
    // If the executable is named 'watchman_test', pass that name:
    g_state = backtrace_create_state("watchman_test", /*threaded=*/1,
                                     error_callback, NULL);

    // 2) Install SIGUSR2 handler
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigusr2_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &sa, NULL);

    // 3) Start a worker thread
    pthread_t tid;
    pthread_create(&tid, NULL, thread_func, NULL);

    // 4) Wait for the thread (which effectively never ends)
    pthread_join(tid, NULL);

    fprintf(stderr, "[watchman_test] done. Exiting main.\n");
    return 0;
}
