#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <linux/ptrace.h>  // For PTRACE_EVENT definitions

#define MAX_WATCHPOINTS 4
#define MAX_THREADS 1024

typedef struct {
    uintptr_t addr;
    size_t size;
} watchpoint_t;

watchpoint_t watchpoints[MAX_WATCHPOINTS];
int num_watchpoints = 0;

pid_t thread_ids[MAX_THREADS];
int num_threads = 0;

#ifdef __x86_64__
#define DEBUGREG_OFFSET(n)  (8 * (n))
#else
#define DEBUGREG_OFFSET(n)  (4 * (n))
#endif

static int set_watchpoint(pid_t tid, int which, uintptr_t addr, size_t length) {
    // Validate which debug register to use (0-3)
    if (which < 0 || which > 3) {
        fprintf(stderr, "[watchman] Invalid debug register: DR%d\n", which);
        return -1;
    }

    // Set the address in DRx
    unsigned long offset = offsetof(struct user, u_debugreg[which]);
    if (ptrace(PTRACE_POKEUSER, tid, (void*)offset, (void*)addr) == -1) {
        fprintf(stderr, "[watchman] TID=%d: Failed to set DR%d to 0x%lx: %s\n",
                tid, which, addr, strerror(errno));
        return -1;
    }

    // Read current DR7
    errno = 0;
    offset = offsetof(struct user, u_debugreg[7]);
    unsigned long dr7 = ptrace(PTRACE_PEEKUSER, tid, (void*)offset, 0);
    if (dr7 == -1 && errno) {
        fprintf(stderr, "[watchman] TID=%d: Failed to read DR7: %s\n", tid, strerror(errno));
        return -1;
    }

    // Enable the local exact breakpoint enable
    dr7 |= (1 << (which * 2));  // Local enable for DRx

    unsigned long rw_bits = 0b01;  // Break on write
    unsigned long len_bits;
    switch (length) {
        case 1: len_bits = 0b00; break;
        case 2: len_bits = 0b01; break;
        case 4: len_bits = 0b11; break;
        default:
            fprintf(stderr, "[watchman] Invalid length %zu. Must be 1, 2, or 4.\n", length);
            return -1;
    }

    unsigned long rw_len = (rw_bits << 2) | len_bits;

    // Clear the existing settings for this breakpoint
    dr7 &= ~((0xF) << (16 + which * 4));

    // Set the new settings
    dr7 |= (rw_len) << (16 + which * 4);

    // Write back DR7
    offset = offsetof(struct user, u_debugreg[7]);
    if (ptrace(PTRACE_POKEUSER, tid, (void*)offset, (void*)dr7) == -1) {
        fprintf(stderr, "[watchman] TID=%d: Failed to set DR7 to 0x%lx: %s\n",
                tid, dr7, strerror(errno));
        return -1;
    }

    fprintf(stderr, "[watchman] TID=%d: Set watchpoint %d at addr=0x%lx with DR7=0x%lx\n",
            tid, which, addr, dr7);

    return 0;
}

int attach_thread(pid_t tid) {
    fprintf(stderr, "[watchman] Trying to attach to TID=%d\n", tid);
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL) == -1) {
        fprintf(stderr, "[watchman] TID=%d PTRACE_ATTACH fail: %s\n", tid, strerror(errno));
        return -1;
    }

    int status;
    if (waitpid(tid, &status, __WALL) == -1) {
        fprintf(stderr, "[watchman] waitpid(%d) fail: %s\n", tid, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[watchman] TID=%d stopped, status=0x%x\n", tid, status);

    if (ptrace(PTRACE_SETOPTIONS, tid, NULL, PTRACE_O_TRACECLONE | PTRACE_O_TRACEEXIT) == -1) {
        fprintf(stderr, "[watchman] TID=%d PTRACE_SETOPTIONS fail: %s\n", tid, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[watchman] TID=%d options set\n", tid);

    // Set all watchpoints for this thread
    for (int i = 0; i < num_watchpoints; i++) {
        if (set_watchpoint(tid, i, watchpoints[i].addr, watchpoints[i].size) == -1) {
            fprintf(stderr, "[watchman] TID=%d Failed to set watchpoint %d\n", tid, i);
            return -1;
        }
    }

    if (ptrace(PTRACE_CONT, tid, NULL, NULL) == -1) {
        fprintf(stderr, "[watchman] TID=%d PTRACE_CONT fail: %s\n", tid, strerror(errno));
    } else {
        fprintf(stderr, "[watchman] TID=%d continued\n", tid);
    }

    return 0;
}

int attach_all_threads(pid_t pid) {
    char tasks_path[256];
    snprintf(tasks_path, sizeof(tasks_path), "/proc/%d/task", pid);

    DIR *dir = opendir(tasks_path);
    if (!dir) {
        perror("opendir");
        return -1;
    }

    struct dirent *entry;
    num_threads = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        pid_t tid = atoi(entry->d_name);
        if (attach_thread(tid) == -1) {
            fprintf(stderr, "[watchman] Failed to attach to TID=%d\n", tid);
            closedir(dir);
            return -1;
        }
        thread_ids[num_threads++] = tid;
    }
    closedir(dir);
    return 0;
}

void handle_trace_event(pid_t tid, int status) {
    if (WIFEXITED(status)) {
        fprintf(stderr, "[watchman] TID=%d exited\n", tid);
    } else if (WIFSTOPPED(status)) {
        int sig = WSTOPSIG(status);
        if (sig == SIGTRAP) {
            siginfo_t siginfo;
            if (ptrace(PTRACE_GETSIGINFO, tid, NULL, &siginfo) == -1) {
                fprintf(stderr, "[watchman] TID=%d PTRACE_GETSIGINFO fail: %s\n", tid, strerror(errno));
                return;
            }

            if (siginfo.si_code == TRAP_HWBKPT) {
                fprintf(stderr, "[watchman] TID=%d: Watchpoint hit at address 0x%lx\n", tid, (unsigned long)siginfo.si_addr);

                // Send SIGUSR2 to the traced process
                if (ptrace(PTRACE_CONT, tid, NULL, (void *)(long)SIGUSR2) == -1) {
                    fprintf(stderr, "[watchman] TID=%d PTRACE_CONT with SIGUSR2 fail: %s\n", tid, strerror(errno));
                }
            } else {
                // Handle other SIGTRAP cases
                if (ptrace(PTRACE_CONT, tid, NULL, NULL) == -1) {
                    fprintf(stderr, "[watchman] TID=%d PTRACE_CONT fail: %s\n", tid, strerror(errno));
                }
            }
        } else {
            // Forward other signals
            fprintf(stderr, "[watchman] TID=%d received signal %d (%s)\n", tid, sig, strsignal(sig));
            if (ptrace(PTRACE_CONT, tid, NULL, (void *)(long)sig) == -1) {
                fprintf(stderr, "[watchman] TID=%d PTRACE_CONT fail: %s\n", tid, strerror(errno));
            }
        }
    } else {
        fprintf(stderr, "[watchman] TID=%d stopped with unexpected status 0x%x\n", tid, status);
        // Continue the process
        if (ptrace(PTRACE_CONT, tid, NULL, NULL) == -1) {
            fprintf(stderr, "[watchman] TID=%d PTRACE_CONT fail: %s\n", tid, strerror(errno));
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 4 || argc % 2 != 0) {
        fprintf(stderr, "Usage: %s <pid> <address1> <length1> [<address2> <length2> ...]\n", argv[0]);
        return 1;
    }

    pid_t pid = atoi(argv[1]);

    // Parse watchpoints
    num_watchpoints = 0;
    for (int i = 2; i < argc; i += 2) {
        if (num_watchpoints >= MAX_WATCHPOINTS) {
            fprintf(stderr, "[watchman] Maximum number of watchpoints (%d) exceeded\n", MAX_WATCHPOINTS);
            return 1;
        }

        watchpoints[num_watchpoints].addr = strtoul(argv[i], NULL, 0);
        watchpoints[num_watchpoints].size = atoi(argv[i + 1]);
        
        if (watchpoints[num_watchpoints].size != 1 &&
            watchpoints[num_watchpoints].size != 2 &&
            watchpoints[num_watchpoints].size != 4) {
            fprintf(stderr, "[watchman] Invalid watchpoint size %zu for watchpoint %d. Must be 1, 2, or 4.\n",
                    watchpoints[num_watchpoints].size, num_watchpoints);
            return 1;
        }

        // Check alignment
        if (watchpoints[num_watchpoints].addr % watchpoints[num_watchpoints].size != 0) {
            fprintf(stderr, "[watchman] Address 0x%lx is not aligned to %zu bytes for watchpoint %d.\n",
                    watchpoints[num_watchpoints].addr, watchpoints[num_watchpoints].size, num_watchpoints);
            return 1;
        }

        num_watchpoints++;
    }

    // Print watchpoints
    fprintf(stderr, "[watchman] Setting %d watchpoint(s):\n", num_watchpoints);
    for (int i = 0; i < num_watchpoints; i++) {
        fprintf(stderr, "  Watchpoint %d: addr=0x%lx, size=%zu\n",
                i, watchpoints[i].addr, watchpoints[i].size);
    }

    // Attach to all threads
    if (attach_all_threads(pid) == -1) {
        fprintf(stderr, "[watchman] Failed to attach to all threads\n");
        return 1;
    }

    fprintf(stderr, "[watchman] Starting main event loop...\n");

    // Main event loop
    while (1) {
        int status;
        pid_t tid = waitpid(-1, &status, __WALL);
        if (tid == -1) {
            if (errno == EINTR)
                continue;
            perror("[watchman] waitpid");
            break;
        }

        handle_trace_event(tid, status);
    }

    fprintf(stderr, "[watchman] Exiting...\n");
    return 0;
}