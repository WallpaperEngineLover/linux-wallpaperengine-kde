#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

static void die (int sig) {
    void* frames[48];
    const char msg[] = "\nnocore: fatal signal, stack:\n";
    write (2, msg, sizeof (msg) - 1);
    backtrace_symbols_fd (frames, backtrace (frames, 48), 2);
    _exit (128 + sig);
}

__attribute__ ((constructor)) static void install (void) {
    struct sigaction action;
    memset (&action, 0, sizeof (action));
    action.sa_handler = die;
    sigemptyset (&action.sa_mask);
    action.sa_flags = SA_NODEFER;
    int signals[] = { SIGABRT, SIGSEGV, SIGBUS, SIGFPE, SIGILL };
    for (unsigned i = 0; i < sizeof (signals) / sizeof (signals[0]); i++) {
	sigaction (signals[i], &action, NULL);
    }
}
