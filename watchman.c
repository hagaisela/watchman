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

#define MAX_WATCHPOINTS 4

typedef struct {
    uintptr_t addr;
    size_t size;
} watch_t;

static watch_t g_watches[MAX_WATCHPOINTS];
static int g_num_watches = 0;
static pid_t g_target_pid = 0;

// ctrl+c flags
static volatile int g_stop_requested = 0;
static volatile int g_already_detaching = 0; // so we ignore subsequent Ctrl+C

static void sigint_handler(int signo) {
    (void)signo;
    // If we're already detaching, ignore further Ctrl+C
    if (g_already_detaching) {
        fprintf(stderr, "[watchman] (Ctrl+C again) ignoring while detaching...\n");
        return;
    }
    g_stop_requested = 1;
}

static void ignore_subsequent_ctrl_c(void) {
    // we set g_already_detaching so further SIGINT does nothing
    g_already_detaching = 1;
}

// DR7: write-only
static unsigned long dr7_rw_bits(void) {
    return 0b01UL;
}

// length => 1=>0b00,2=>0b01,4=>0b11,8=>0b10
static unsigned long dr7_length_bits(size_t length) {
    switch(length) {
    case 1: return 0b00UL;
    case 2: return 0b01UL;
    case 4: return 0b11UL;
    case 8: return 0b10UL;
    default:
        fprintf(stderr, "[watchman] forcing length=4 for %zu\n", length);
        return 0b11UL;
    }
}

static int attach_thread(pid_t tid) {
    if (ptrace(PTRACE_ATTACH, tid, NULL, NULL)==-1) {
        fprintf(stderr, "[watchman] TID=%d PTRACE_ATTACH fail: %s\n",
                tid, strerror(errno));
        return -1;
    }
    int st;
    if (waitpid(tid, &st, __WALL)==-1) {
        fprintf(stderr, "[watchman] waitpid(%d) fail: %s\n", tid, strerror(errno));
        return -1;
    }
    return 0;
}

static int set_watchpoint(pid_t tid, int which, uintptr_t addr, size_t length) {
    off_t off = offsetof(struct user, u_debugreg[0]) + which*sizeof(long);
    if (ptrace(PTRACE_POKEUSER, tid, (void*)off, (void*)addr)==-1) {
        fprintf(stderr,"[watchman] TID=%d: poke DR%d fail:%s\n", tid, which, strerror(errno));
        return -1;
    }

    long dr7 = ptrace(PTRACE_PEEKUSER, tid,(void*)offsetof(struct user, u_debugreg[7]),0);
    if (dr7==-1 && errno) {
        fprintf(stderr,"[watchman] TID=%d: peek DR7 fail:%s\n", tid, strerror(errno));
        return -1;
    }
    // local enable => bit(2*which)
    dr7 |= (1UL<<(2*which));

    unsigned long rw = dr7_rw_bits();
    unsigned long ln = dr7_length_bits(length);
    unsigned long combined = ((rw<<2)| ln) & 0xF;

    unsigned long shift = 16 + which*4;
    unsigned long mask = 0xFUL << shift;
    dr7 &= ~mask;
    dr7 |= (combined<<shift);

    if (ptrace(PTRACE_POKEUSER, tid,(void*)offsetof(struct user,u_debugreg[7]),
               (void*)dr7)==-1) {
        fprintf(stderr,"[watchman] TID=%d: poke DR7 fail:%s\n", tid, strerror(errno));
        return -1;
    }
    return 0;
}

// flush leftover watchpoint traps by single-stepping
// up to e.g. 5 times
static void flush_thread(pid_t tid) {
    // Clear DR7 => no watchpoints
    ptrace(PTRACE_POKEUSER, tid,(void*)offsetof(struct user,u_debugreg[7]),
           (void*)0);

    // Also clear DR0..DR3
    for (int i=0; i<4; i++) {
        off_t off = offsetof(struct user,u_debugreg[0]) + i*sizeof(long);
        ptrace(PTRACE_POKEUSER, tid, (void*)off, (void*)0);
    }

    // We'll do up to 5 single-steps or until we see no watch_bits
    for (int step=0; step<5; step++) {
        if (ptrace(PTRACE_SINGLESTEP, tid, NULL, 0)==-1) {
            fprintf(stderr,"[watchman] TID=%d: SINGLESTEP fail:%s\n", tid, strerror(errno));
            return;
        }
        int st;
        waitpid(tid, &st, __WALL);

        if (!WIFSTOPPED(st)) {
            // The thread might have exited?
            return;
        }
        if (WSTOPSIG(st) != SIGTRAP) {
            // no trap => leftover signal
            continue;
        }
        // check DR6
        long dr6 = ptrace(PTRACE_PEEKUSER, tid,
                          (void*)offsetof(struct user,u_debugreg[6]),
                          0);
        int watch_bits = dr6 & 0xF;
        // clear DR6
        ptrace(PTRACE_POKEUSER, tid,
               (void*)offsetof(struct user,u_debugreg[6]),
               (void*)0);

        if (!watch_bits) {
            // no hardware watchpoint triggered => we might be safe
            // but we do all 5 steps or break early. Let's break early:
            break;
        }
        // if watch_bits != 0 => watchpoint triggered again
        // but we already set DR7=0, so it might be a leftover queued trap
        // keep single-stepping
    }
}

// final detach for one TID
static void detach_thread(pid_t tid) {
    // do PTRACE_DETACH
    if (ptrace(PTRACE_DETACH, tid, NULL, NULL)==-1) {
        fprintf(stderr,"[watchman] TID=%d: DETACH fail:%s\n", tid, strerror(errno));
    } else {
        fprintf(stderr,"[watchman] Detached TID=%d\n", tid);
    }
}

// main function for final detach
// 1) send SIGSTOP => fully freeze the process
// 2) interrupt each TID => flush => detach
// 3) optional SIGCONT
static void flush_and_detach_all_threads(pid_t pid, int do_sigcont_after) {
    // Freeze entire process
    kill(pid, SIGSTOP);

    // Wait for main thread to stop
    int st;
    if (waitpid(pid, &st, WUNTRACED)==-1) {
        fprintf(stderr,"[watchman] waitpid(%d) fail:%s\n", pid, strerror(errno));
        // continue anyway
    } else {
        if (WIFSTOPPED(st)) {
            fprintf(stderr,"[watchman] pid=%d is SIGSTOPped.\n", pid);
        }
    }

    // Now enumerate TIDs
    char dirpath[64];
    snprintf(dirpath,sizeof(dirpath),"/proc/%d/task", pid);

    DIR* d = opendir(dirpath);
    if (!d) {
        fprintf(stderr,"[watchman] cannot open %s:%s\n", dirpath, strerror(errno));
        return;
    }
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0]=='.') continue;
        pid_t tid = atoi(de->d_name);
        if (!tid) continue;

        // we assume tid was attached since the start => no re-attach
        // but we do PTRACE_INTERRUPT just in case it's not truly in ptrace-stop
        if (ptrace(PTRACE_INTERRUPT, tid, NULL, NULL)==0) {
            int st2;
            waitpid(tid, &st2, __WALL);
        }
        // flush
        flush_thread(tid);
        // detach
        detach_thread(tid);
    }
    closedir(d);

    if (do_sigcont_after) {
        kill(pid, SIGCONT);
        fprintf(stderr,"[watchman] Sent SIGCONT pid=%d\n",pid);
    }
}

int main(int argc, char *argv[]) {
    if (argc<4) {
        fprintf(stderr,"Usage:%s <pid> <addr1> <size1> [<addr2> <size2> ...]\n",argv[0]);
        return 1;
    }
    g_target_pid = atoi(argv[1]);
    int pairs = (argc-2)/2;
    if (pairs>MAX_WATCHPOINTS) pairs=MAX_WATCHPOINTS;

    for(int i=0;i<pairs;i++){
        uintptr_t a = strtoull(argv[2+i*2], NULL, 16);
        size_t sz = (size_t)strtoull(argv[3+i*2], NULL, 0);
        g_watches[i].addr = a;
        g_watches[i].size = sz;
        g_num_watches++;
        fprintf(stderr,"[watchman] watch #%d => addr=0x%lx, size=%zu\n", i,(unsigned long)a,sz);
    }

    // install ctrl+c
    struct sigaction sa;
    memset(&sa,0,sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT,&sa,NULL);

    // attach existing TIDs
    // then we let them run
    {
        char dirpath[64];
        snprintf(dirpath,sizeof(dirpath),"/proc/%d/task",g_target_pid);
        DIR* dd = opendir(dirpath);
        if(dd){
            struct dirent *xx;
            while ((xx=readdir(dd))) {
                if(xx->d_name[0]=='.') continue;
                pid_t tid = atoi(xx->d_name);
                if(!tid) continue;
                fprintf(stderr,"[watchman] attaching TID=%d\n", tid);
                if(attach_thread(tid)==0){
                    for(int i=0; i<g_num_watches && i<4; i++){
                        set_watchpoint(tid,i,g_watches[i].addr,g_watches[i].size);
                    }
                    ptrace(PTRACE_CONT,tid,NULL,NULL);
                }
            }
            closedir(dd);
        }
    }

    // main event loop
    while(!g_stop_requested){
        int status;
        pid_t tid = waitpid(-1,&status,__WALL);
        if(tid<0){
            if(errno==EINTR && !g_stop_requested) continue;
            break;
        }
        if(WIFSTOPPED(status)){
            int sig = WSTOPSIG(status);
            if(sig==SIGTRAP){
                long dr6 = ptrace(PTRACE_PEEKUSER,tid,(void*)offsetof(struct user,u_debugreg[6]),0);
                int watch_bits = dr6 & 0xF;
                int step_bit = dr6 & (1<<14);
                if(watch_bits){
                    // clear dr6
                    ptrace(PTRACE_POKEUSER,tid,(void*)offsetof(struct user,u_debugreg[6]),(void*)0);
                    // deliver SIGUSR2
                    ptrace(PTRACE_CONT,tid,NULL,(void*)SIGUSR2);
                }else if(step_bit){
                    ptrace(PTRACE_CONT,tid,NULL,0);
                }else{
                    ptrace(PTRACE_CONT,tid,NULL,(void*)(long)sig);
                }
            } else{
                // pass signal
                ptrace(PTRACE_CONT,tid,NULL,(void*)(long)sig);
            }
        }else if(WIFEXITED(status)||WIFSIGNALED(status)){
            if(tid==g_target_pid){
                fprintf(stderr,"[watchman] Target pid %d exited\n", g_target_pid);
                break;
            } else{
                fprintf(stderr,"[watchman] TID=%d exited\n", tid);
            }
        }
    }

    // we do final detach
    // ignore subsequent ctrl+c
    ignore_subsequent_ctrl_c();

    fprintf(stderr,"[watchman] shutting down => flush + detach...\n");
    flush_and_detach_all_threads(g_target_pid, 1);
    fprintf(stderr,"[watchman] done.\n");
    return 0;
}