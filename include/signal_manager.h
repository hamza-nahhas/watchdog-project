#ifndef SIGNAL_MANAGER_H
#define SIGNAL_MANAGER_H

#include <signal.h>

/* Set ONLY by signal handlers; read and cleared ONLY by the main loop. */
extern volatile sig_atomic_t g_sigchld;   /* 1 = at least one child changed state         */
extern volatile sig_atomic_t g_sigusr1;   /* 1 = status report requested                  */
extern volatile sig_atomic_t g_sigalrm;   /* 1 = periodic timer fired                     */
extern volatile sig_atomic_t g_shutdown;  /* 0 = keep running; otherwise SIGINT or SIGTERM */

/* Block SIGCHLD, SIGUSR1, SIGALRM, SIGINT, SIGTERM. Previous mask -> *oldmask.
 * Call BEFORE installing handlers and BEFORE forking any child. Returns 0 or -1. */
int sm_block_signals(sigset_t *oldmask);

/* Install handlers for the 5 signals with sigaction().
 * sa_mask blocks all 5 while a handler runs; SA_RESTART on all;
 * SA_NOCLDSTOP on SIGCHLD. Returns 0 or -1. */
int sm_install_handlers(void);

/* waitmask = oldmask with the 5 signals removed. Pass waitmask to sigsuspend(). */
void sm_wait_mask(const sigset_t *oldmask, sigset_t *waitmask);

/* Call in the CHILD between fork() and exec(): reset the 5 signals to SIG_DFL
 * and unblock ALL signals (a blocked mask survives exec!). */
void sm_child_reset(void);

/* "SIGINT", "SIGTERM", "SIGKILL", ... for log messages. Not for use in handlers. */
const char *sm_signal_name(int signo);

#endif /* SIGNAL_MANAGER_H */
