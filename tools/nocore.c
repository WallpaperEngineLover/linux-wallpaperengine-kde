// nocore <cmd> [args]: runs a command and all its descendants under ptrace and kills any of them that gets a
// fatal signal (SEGV, ABRT, BUS, ILL, FPE) with SIGKILL before the kernel starts a core dump. RLIMIT_CORE=0 does not
// help with a core_pattern that pipes to systemd-coredump, the handler still runs and the desktop shows a crash report.
// gcc -O2 -o nocore tools/nocore.c
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: nocore <cmd> [args]\n");
        return 2;
    }
    pid_t root = fork();
    if (root < 0) {
        perror("fork");
        return 2;
    }
    if (root == 0) {
        ptrace(PTRACE_TRACEME, 0, 0, 0);
        execvp(argv[1], argv + 1);
        perror(argv[1]);
        _exit(127);
    }
    int rootStatus = 0;
    int first = 1;
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, __WALL);
        if (pid < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (pid == root)
                rootStatus = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;
        if (first && pid == root) {
            ptrace(PTRACE_SETOPTIONS, pid, 0,
                   PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK | PTRACE_O_TRACECLONE | PTRACE_O_EXITKILL);
            first = 0;
        }
        int sig = WSTOPSIG(status);
        if (status >> 16 || sig == SIGTRAP || sig == SIGSTOP) {
            ptrace(PTRACE_CONT, pid, 0, 0);
        } else if (sig == SIGSEGV || sig == SIGABRT || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
            fprintf(stderr, "nocore: pid %d got signal %d, killed without a core dump\n", pid, sig);
            kill(pid, SIGKILL);
            ptrace(PTRACE_CONT, pid, 0, SIGKILL);
        } else {
            ptrace(PTRACE_CONT, pid, 0, sig);
        }
    }
    return rootStatus;
}
