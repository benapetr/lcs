// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_agreement_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* Private same-host pipe format, never a network message. */
typedef struct
{
    int result;
    uint32_t length;
    bool poisoned;
    char error[256];
} worker_result_t;

#define RESULT_MAX (sizeof(worker_result_t) + LCS_CONFIG_JOURNAL_MAX)

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "configuration storage worker: %s", message);
    return -1;
}

void lcs_config_agreement_worker_init(lcs_config_agreement_worker_t *worker)
{
    memset(worker, 0, sizeof(*worker));
    worker->pipe_fd = -1;
}

static void reset(lcs_config_agreement_worker_t *worker)
{
    if (worker->pipe_fd >= 0)
        close(worker->pipe_fd);
    free(worker->buffer);
    lcs_config_agreement_worker_init(worker);
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    while (length)
    {
        ssize_t written = write(fd, bytes, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

static int compare_fd(const void *a, const void *b)
{
    int left = *(const int *)a, right = *(const int *)b;
    return (left > right) - (left < right);
}

static _Noreturn void execute_child(lcs_config_agreement_t *agreement,
                            const lcs_config_agreement_job_t *job,
                            int pipe_fd, pid_t parent)
{
    int keep[] = {agreement->store->directory_fd, agreement->store->lock_fd, pipe_fd};
    qsort(keep, 3, sizeof(keep[0]), compare_fd);
    unsigned first = 3;
    for (size_t i = 0; i < 3; i++)
    {
        if (keep[i] < 3)
            continue;
        if (first < (unsigned)keep[i])
            (void)close_range(first, (unsigned)keep[i] - 1, 0);
        first = (unsigned)keep[i] + 1;
    }
    (void)close_range(first, ~0u, 0);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        _exit(1);
    worker_result_t result = {0};
    if (job->action == LCS_CONFIG_PREPARE)
        result.result = lcs_config_agreement_prepare(agreement, job->revision,
            job->parent_digest, job->ballot, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_ACCEPT)
        result.result = lcs_config_agreement_accept(agreement, job->parent_digest,
            job->ballot, job->value, result.error, sizeof(result.error));
    else
        result.result = lcs_config_agreement_learn(agreement, job->parent_digest,
            job->ballot, job->voters, job->value, result.error, sizeof(result.error));
    result.poisoned = agreement->poisoned;
    char *record = NULL;
    size_t length = 0;
    if (!result.poisoned && lcs_config_agreement_export(agreement, &record, &length,
                                                        result.error, sizeof(result.error)))
    {
        result.poisoned = true;
        result.result = -1;
    }
    result.length = (uint32_t)length;
    if (write_all(pipe_fd, &result, sizeof(result)) ||
        (length && write_all(pipe_fd, record, length)))
        _exit(1);
    free(record);
    _exit(0);
}

int lcs_config_agreement_worker_start(lcs_config_agreement_worker_t *worker,
                                      lcs_config_agreement_t *agreement,
                                      const lcs_config_agreement_job_t *job,
                                      char *err, size_t err_len)
{
    if (worker->pid > 0)
        return 1;
    if (agreement->poisoned || !job || job->action < LCS_CONFIG_PREPARE ||
        job->action > LCS_CONFIG_LEARN || (job->action != LCS_CONFIG_PREPARE && !job->value))
        return fail(err, err_len, "invalid job or agreement state requires recovery");
    unsigned char *buffer = malloc(RESULT_MAX);
    if (!buffer)
        return fail(err, err_len, "out of memory allocating bounded result");
    int fds[2];
    if (pipe2(fds, O_CLOEXEC))
    {
        free(buffer);
        return fail(err, err_len, "cannot create result pipe");
    }
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK))
    {
        close(fds[0]); close(fds[1]); free(buffer);
        return fail(err, err_len, "cannot make result pipe nonblocking");
    }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
    {
        close(fds[0]); close(fds[1]); free(buffer);
        return fail(err, err_len, "cannot fork storage worker");
    }
    if (!child)
        execute_child(agreement, job, fds[1], parent);
    close(fds[1]);
    worker->buffer = buffer;
    worker->pipe_fd = fds[0];
    worker->pid = child;
    worker->used = 0;
    worker->eof = false;
    worker->cancelled = false;
    return 0;
}

void lcs_config_agreement_worker_cancel(lcs_config_agreement_worker_t *worker,
                                        lcs_config_agreement_t *agreement)
{
    if (worker->pid > 0)
    {
        agreement->poisoned = true;
        worker->cancelled = true;
        pid_t reaped;
        do
        {
            reaped = waitpid(worker->pid, NULL, WNOHANG);
        } while (reaped < 0 && errno == EINTR);
        if (!reaped)
        {
            kill(worker->pid, SIGKILL);
            do
            {
                reaped = waitpid(worker->pid, NULL, WNOHANG);
            } while (reaped < 0 && errno == EINTR);
        }
        if (!reaped || (reaped < 0 && errno == EINTR))
            return;
    }
    reset(worker);
}

int lcs_config_agreement_worker_poll(lcs_config_agreement_worker_t *worker,
                                     lcs_config_agreement_t *agreement,
                                     int *operation_result,
                                     char *err, size_t err_len)
{
    if (worker->pid <= 0)
        return 0;
    if (worker->cancelled)
    {
        pid_t reaped = waitpid(worker->pid, NULL, WNOHANG);
        if (!reaped || (reaped < 0 && errno == EINTR))
            return 0;
        agreement->poisoned = true;
        reset(worker);
        return fail(err, err_len, "cancelled worker has exited; recover the journal before voting");
    }
    /* Bound scheduler work per call even when a large result is available. */
    size_t budget = 64u * 1024u;
    while (!worker->eof && budget)
    {
        if (worker->used == RESULT_MAX)
            break;
        size_t count = RESULT_MAX - worker->used;
        if (count > budget)
            count = budget;
        ssize_t n = read(worker->pipe_fd, worker->buffer + worker->used, count);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0)
            goto broken;
        if (!n)
        {
            worker->eof = true;
            break;
        }
        worker->used += (size_t)n;
        budget -= (size_t)n;
    }
    if (!worker->eof && worker->used < RESULT_MAX)
        return 0;
    int status;
    pid_t pid = waitpid(worker->pid, &status, WNOHANG);
    if (!pid || (pid < 0 && errno == EINTR))
        return 0;
    /* Never signal a PID after it has been reaped, even for a malformed result. */
    worker->pid = 0;
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) || worker->used < sizeof(worker_result_t))
        goto broken;
    worker_result_t result;
    memcpy(&result, worker->buffer, sizeof(result));
    result.error[sizeof(result.error) - 1] = '\0';
    if (result.length > LCS_CONFIG_JOURNAL_MAX ||
        worker->used != sizeof(result) + result.length ||
        result.result < -1 || result.result > LCS_CONFIG_STALE)
        goto broken;
    if (result.poisoned)
    {
        agreement->poisoned = true;
        fail(err, err_len, *result.error ? result.error : "uncertain persistence; recovery required");
        reset(worker);
        return -1;
    }
    if (!result.length || lcs_config_agreement_import(agreement,
            (const char *)worker->buffer + sizeof(result), result.length, err, err_len))
    {
        agreement->poisoned = true;
        reset(worker);
        return -1;
    }
    *operation_result = result.result;
    if (result.result < 0)
        fail(err, err_len, result.error);
    reset(worker);
    return 1;
broken:
    agreement->poisoned = true;
    lcs_config_agreement_worker_cancel(worker, agreement);
    return fail(err, err_len, "worker failed before a complete durable result; recover before voting");
}
