/*
 * signal_manager.c - async-signal-safe signal handling for the watchdog.
 *
 * Design (CONTRACT.md section 3, rules 2-4):
 *   - Handlers do exactly one thing: store into a volatile sig_atomic_t flag.
 *     All real work (waitpid, logging, fork) happens later in the main loop.
 *   - The main loop keeps the 5 signals blocked and only lets them in inside
 *     sigsuspend(), so a flag can never be set between "check" and "sleep".
 */
#include "signal_manager.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

/* volatile: the compiler must re-read the flag on every check, because it is
 * changed outside normal control flow. sig_atomic_t: reads and writes are
 * atomic with respect to signal delivery, so the main loop never sees a
 * half-written value. These are the only object types the C standard lets a
 * handler write to safely. */
volatile sig_atomic_t g_sigchld  = 0;
volatile sig_atomic_t g_sigusr1  = 0;
volatile sig_atomic_t g_sigalrm  = 0;
volatile sig_atomic_t g_shutdown = 0;

/* The 5 signals the watchdog handles; shared by every helper below. */
static const int k_signals[] = { SIGCHLD, SIGUSR1, SIGALRM, SIGINT, SIGTERM };
#define N_SIGNALS ((int)(sizeof k_signals / sizeof k_signals[0]))

/* One handler for all 5 signals. It calls no function at all (so nothing
 * non-async-signal-safe can slip in) and only does plain stores into
 * sig_atomic_t flags. errno is saved and restored anyway, so that a future
 * edit adding a syscall here cannot corrupt the errno the interrupted main
 * code is about to read. */
static void sm_handler(int signo)
{
    int saved_errno = errno;

    switch (signo) {
    case SIGCHLD: g_sigchld = 1;      break;
    case SIGUSR1: g_sigusr1 = 1;      break;
    case SIGALRM: g_sigalrm = 1;      break;
    case SIGINT:                       /* fall through */
    case SIGTERM: g_shutdown = signo; break; /* keep the number: SHUTDOWN_BEGIN logs it */
    default:                          break;
    }

    errno = saved_errno;
}

int sm_install_handlers(void)
{
    struct sigaction sa;
    int i;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sm_handler;

    /* sa_mask blocks all 5 while the handler runs, so one handler can never be
     * interrupted by another one (no nesting, no surprises). */
    if (sigemptyset(&sa.sa_mask) == -1)
        return -1;
    for (i = 0; i < N_SIGNALS; i++)
        if (sigaddset(&sa.sa_mask, k_signals[i]) == -1)
            return -1;

    for (i = 0; i < N_SIGNALS; i++) {
        /* SA_RESTART: slow syscalls (write to the log, waitpid, ...) are
         * resumed instead of failing with EINTR. sigsuspend() and nanosleep()
         * still return EINTR, which is exactly what the main loop wants. */
        sa.sa_flags = SA_RESTART;
        /* SA_NOCLDSTOP: no SIGCHLD when a child is merely stopped/continued
         * (SIGSTOP/SIGCONT); we only care about children that terminate. */
        if (k_signals[i] == SIGCHLD)
            sa.sa_flags |= SA_NOCLDSTOP;
        /* sigaction() rather than signal(): its semantics are fully defined by
         * POSIX (handler stays installed, mask and flags are explicit). */
        if (sigaction(k_signals[i], &sa, NULL) == -1)
            return -1;
    }
    return 0;
}

int sm_block_signals(sigset_t *oldmask)
{
    sigset_t set;
    int i;

    if (sigemptyset(&set) == -1)
        return -1;
    for (i = 0; i < N_SIGNALS; i++)
        if (sigaddset(&set, k_signals[i]) == -1)
            return -1;

    /* Blocked BEFORE handlers are installed and BEFORE any fork(): a child
     * that dies instantly then leaves SIGCHLD pending instead of firing while
     * the child table is half filled in. Pending signals are delivered at the
     * next sigsuspend(), so nothing is lost. The previous mask is returned so
     * the wait mask can be built from it. */
    return sigprocmask(SIG_BLOCK, &set, oldmask);
}

void sm_wait_mask(const sigset_t *oldmask, sigset_t *waitmask)
{
    int i;

    /* waitmask = the original mask with our 5 signals allowed. sigsuspend()
     * installs this mask and sleeps in ONE atomic step, so a signal arriving
     * just after the main loop checked the flags stays pending until we are
     * asleep and then wakes us: the classic "check, then pause()" race where
     * the wakeup is lost cannot happen. sigdelset() only fails for an invalid
     * signal number, and these 5 are always valid. */
    *waitmask = *oldmask;
    for (i = 0; i < N_SIGNALS; i++)
        (void)sigdelset(waitmask, k_signals[i]);
}

/* Runs in the child between fork() and exec(). After fork() only
 * async-signal-safe calls are allowed (another thread or a stdio buffer of the
 * parent could be mid-update), so only sigaction, sigemptyset, sigprocmask and
 * write are used here: no stdio, no logger.
 * On failure there is no one to return an error to, so a fixed message goes
 * to stderr with write() and the exec still happens. */
void sm_child_reset(void)
{
    static const char msg[] = "watchdog: sm_child_reset failed in child\n";
    struct sigaction sa;
    sigset_t none;
    int i, failed = 0;

    /* Handlers would be reset by exec anyway, but resetting them here also
     * covers the window between fork and exec: a signal there must not run
     * the parent's handler and set the parent's flags in the child's copy. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    if (sigemptyset(&sa.sa_mask) == -1)
        failed = 1;
    for (i = 0; i < N_SIGNALS; i++)
        if (sigaction(k_signals[i], &sa, NULL) == -1)
            failed = 1;

    /* The signal mask, unlike handlers, SURVIVES exec(). Without this the
     * worker would start with SIGTERM blocked and ignore the watchdog's
     * shutdown request. Unblock everything. */
    if (sigemptyset(&none) == -1 || sigprocmask(SIG_SETMASK, &none, NULL) == -1)
        failed = 1;

    if (failed) {
        ssize_t r = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void)r; /* nothing more can be done in the child */
    }
}

/* For log messages only (uses no state, but not meant for handlers: logging
 * never happens in a handler anyway). 0 means "no signal" and gives "NONE",
 * which is what SHUTDOWN_BEGIN logs when all children finished on their own. */
const char *sm_signal_name(int signo)
{
    switch (signo) {
    case 0:       return "NONE";
    case SIGHUP:  return "SIGHUP";
    case SIGINT:  return "SIGINT";
    case SIGQUIT: return "SIGQUIT";
    case SIGILL:  return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGABRT: return "SIGABRT";
    case SIGBUS:  return "SIGBUS";
    case SIGFPE:  return "SIGFPE";
    case SIGKILL: return "SIGKILL";
    case SIGUSR1: return "SIGUSR1";
    case SIGSEGV: return "SIGSEGV";
    case SIGUSR2: return "SIGUSR2";
    case SIGPIPE: return "SIGPIPE";
    case SIGALRM: return "SIGALRM";
    case SIGTERM: return "SIGTERM";
    case SIGCHLD: return "SIGCHLD";
    case SIGCONT: return "SIGCONT";
    case SIGSTOP: return "SIGSTOP";
    case SIGTSTP: return "SIGTSTP";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    default:      return "UNKNOWN";
    }
}
