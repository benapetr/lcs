// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_WORKER_H
#define LCS_WORKER_H
#include <errno.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* Backend workers do not exec themselves, so their parent-death handler can
 * terminate the entire process group, including any external helper commands. */
static inline void lcs_worker_parent_lost(int signal_number)
{
    (void)signal_number;
    (void)kill(-getpid(), SIGKILL);
    _exit(125);
}

/* Child only, before any backend action. Close the fork/prctl race and avoid
 * letting inherited daemon signal masks suppress parent-death cancellation. */
static inline void lcs_worker_bind_parent(pid_t parent)
{
    struct sigaction action = {0};
    action.sa_handler = lcs_worker_parent_lost;
    sigemptyset(&action.sa_mask);
    sigset_t mask;
    sigemptyset(&mask); sigaddset(&mask, SIGTERM);
    if (setpgid(0, 0) || sigaction(SIGTERM, &action, NULL) ||
        sigprocmask(SIG_UNBLOCK, &mask, NULL) || prctl(PR_SET_PDEATHSIG, SIGTERM))
        _exit(125);
    if (getppid() != parent)
        lcs_worker_parent_lost(SIGTERM);
}

/* Exec resets caught signals. Keep the process-group supervisor alive while
 * the foreground hook and its commands run; cancellation kills the whole group.
 * Hooks must be idempotent foreground commands, not daemon launchers. */
static inline _Noreturn void lcs_worker_exec_hook(const char *path)
{
    pid_t supervisor = getpid(), command = fork();
    if (command < 0) _exit(127);
    if (!command)
    {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != supervisor)
            _exit(125);
        execl(path, path, (char *)NULL);
        _exit(127);
    }
    int status;
    pid_t got;
    do { got = waitpid(command, &status, 0); } while (got < 0 && errno == EINTR);
    _exit(got > 0 && WIFEXITED(status) ? WEXITSTATUS(status) : 1);
}
#endif
