// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_bootstrap.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(char *err, size_t err_len, const char *message)
{
    if (err && err_len)
        snprintf(err, err_len, "resource bootstrap: %s", message);
    return -1;
}

void lcs_config_bootstrap_close(lcs_config_bootstrap_t *bootstrap)
{
    if (bootstrap->worker_pid > 0)
    {
        kill(bootstrap->worker_pid, SIGKILL);
        (void)waitpid(bootstrap->worker_pid, NULL, WNOHANG);
        bootstrap->worker_pid = 0;
    }
    if (bootstrap->worker_pipe >= 0)
        close(bootstrap->worker_pipe);
    bootstrap->worker_pipe = -1;
    lcs_config_store_close(&bootstrap->snapshots);
    lcs_config_store_close(&bootstrap->state);
}

static int marker(char *buffer, size_t cap, const char *cluster, const char *phase)
{
    return snprintf(buffer, cap, "LCS-RESOURCE-BOOTSTRAP-1\n%s\n%s\n", cluster, phase);
}

int lcs_config_bootstrap_refresh(lcs_config_bootstrap_t *bootstrap,
                                 const lcs_config_t *cfg,
                                 char *err, size_t err_len)
{
    char *record = NULL, *current = NULL, *next = NULL;
    size_t record_len = 0, current_len = 0, next_len = 0;
    int rc = -1;
    char preparing[256], prepared[256];
    marker(preparing, sizeof(preparing), cfg->cluster_name, "preparing");
    marker(prepared, sizeof(prepared), cfg->cluster_name, "prepared");
    int record_status = lcs_config_store_read(&bootstrap->state, "bootstrap.state",
        &record, &record_len, 255, err, err_len);
    if (record_status < 0)
        goto out;
    bool initial = record_status == 1;
    bool in_progress = !initial && record_len == strlen(preparing) &&
                       memcmp(record, preparing, record_len) == 0;
    bool ready = !initial && record_len == strlen(prepared) &&
                 memcmp(record, prepared, record_len) == 0;
    if (!initial && !in_progress && !ready)
    {
        fail(err, err_len, "unknown authority, cluster, or initialization record; recover state instead of reimporting INI");
        goto out;
    }
    int current_status = lcs_config_store_read(&bootstrap->snapshots,
        bootstrap->snapshot_name, &current, &current_len, LCS_RESOURCE_DOCUMENT_MAX,
        err, err_len);
    if (current_status < 0)
        goto out;
    if ((initial && current_status == 0) || (ready && current_status == 1))
    {
        fail(err, err_len, "snapshot and initialization record disagree; recover missing state instead of reimporting INI");
        goto out;
    }
    if (current_status == 0)
    {
        lcs_config_t *decoded = malloc(sizeof(*decoded));
        uint64_t revision;
        if (!decoded)
        {
            fail(err, err_len, "out of memory validating snapshot");
            goto out;
        }
        int parsed = lcs_resource_config_decode(current, current_len, cfg, decoded,
                                                &revision, err, err_len);
        free(decoded);
        if (parsed)
            goto out;
        if (revision != 0)
        {
            fail(err, err_len, "non-bootstrap revision requires distributed recovery; refusing INI overwrite");
            goto out;
        }
    }
    if (lcs_resource_config_encode(cfg, 0, &next, &next_len, err, err_len))
        goto out;
    if (initial && lcs_config_store_write(&bootstrap->state, "bootstrap.state",
        preparing, strlen(preparing), err, err_len))
        goto out;
    if ((!current || current_len != next_len || memcmp(current, next, next_len)) &&
        lcs_config_store_write(&bootstrap->snapshots, bootstrap->snapshot_name,
                               next, next_len, err, err_len))
        goto out;
    if (!ready && lcs_config_store_write(&bootstrap->state, "bootstrap.state",
        prepared, strlen(prepared), err, err_len))
        goto out;
    rc = 0;
out:
    free(record);
    free(current);
    free(next);
    return rc;
}

int lcs_config_bootstrap_open(lcs_config_bootstrap_t *bootstrap,
                              const char *config_path, const lcs_config_t *cfg,
                              char *err, size_t err_len)
{
    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->worker_pipe = -1;
    bootstrap->state.directory_fd = bootstrap->state.lock_fd = -1;
    bootstrap->snapshots.directory_fd = bootstrap->snapshots.lock_fd = -1;
    /* Canonicalize the INI path so relative aliases use the same state lock.
     * Custom configurations have isolated sibling files, including fixtures
     * running several nodes in the same directory. */
    char *absolute = realpath(config_path, NULL);
    if (!absolute)
        return fail(err, err_len, "cannot resolve static configuration path");
    char snapshot[PATH_MAX], directory[PATH_MAX];
    int n1, n2;
    if (strcmp(absolute, LCS_DEFAULT_CONFIG_PATH) == 0)
    {
        n1 = snprintf(snapshot, sizeof(snapshot), "/etc/lcs/resources.json");
        n2 = snprintf(directory, sizeof(directory), "/var/lib/lcs");
    }
    else
    {
        n1 = snprintf(snapshot, sizeof(snapshot), "%s.resources.json", absolute);
        n2 = snprintf(directory, sizeof(directory), "%s.state", absolute);
    }
    free(absolute);
    if (n1 >= (int)sizeof(snapshot) || n2 >= (int)sizeof(directory))
        return fail(err, err_len, "storage path is too long");
    if (lcs_config_store_open(&bootstrap->state, directory, err, err_len))
        return -1;
    char *slash = strrchr(snapshot, '/');
    if (!slash || strlen(slash + 1) >= sizeof(bootstrap->snapshot_name))
    {
        fail(err, err_len, "snapshot name is too long");
        goto error;
    }
    snprintf(bootstrap->snapshot_name, sizeof(bootstrap->snapshot_name), "%s", slash + 1);
    *slash = '\0';
    bootstrap->snapshots.directory_fd = open(*snapshot ? snapshot : "/",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (bootstrap->snapshots.directory_fd < 0 ||
        fstat(bootstrap->snapshots.directory_fd, &st) ||
        st.st_uid != geteuid() || (st.st_mode & 0022))
    {
        fail(err, err_len, "snapshot directory must be owned by the daemon user and not writable by group/others");
        goto error;
    }
    /* The state lock protects this instance's snapshot as well as its metadata. */
    if (lcs_config_bootstrap_refresh(bootstrap, cfg, err, err_len))
        goto error;
    bootstrap->prepared = true;
    return 0;
error:
    lcs_config_bootstrap_close(bootstrap);
    return -1;
}

void lcs_config_bootstrap_request(lcs_config_bootstrap_t *bootstrap)
{
    bootstrap->refresh_pending = true;
    bootstrap->prepared = false;
}

static int compare_fd(const void *a, const void *b)
{
    int left = *(const int *)a, right = *(const int *)b;
    return (left > right) - (left < right);
}

int lcs_config_bootstrap_process(lcs_config_bootstrap_t *bootstrap,
                                 const lcs_config_t *cfg,
                                 char *err, size_t err_len)
{
    int result = 0;
    if (bootstrap->worker_pid > 0)
    {
        int status;
        pid_t rc = waitpid(bootstrap->worker_pid, &status, WNOHANG);
        if (rc == 0 || (rc < 0 && errno == EINTR))
            return 0;
        bool succeeded = rc > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (!succeeded)
        {
            char message[256] = "";
            ssize_t n = read(bootstrap->worker_pipe, message, sizeof(message) - 1);
            if (n <= 0)
                snprintf(message, sizeof(message), "background preparation worker failed");
            result = fail(err, err_len, message);
        }
        else
        {
            result = 1;
            bootstrap->prepared = !bootstrap->refresh_pending;
        }
        close(bootstrap->worker_pipe);
        bootstrap->worker_pipe = -1;
        bootstrap->worker_pid = 0;
    }
    if (!bootstrap->refresh_pending)
        return result;
    int pipe_fds[2];
    if (pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK))
        return fail(err, err_len, "cannot create background worker pipe");
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
    {
        close(pipe_fds[0]); close(pipe_fds[1]);
        return fail(err, err_len, "cannot create background preparation worker");
    }
    if (child == 0)
    {
        /* Keep only private state descriptors and the result pipe. In
         * particular, a blocked fsync must not keep cluster sockets alive. */
        int keep[] = {bootstrap->state.directory_fd, bootstrap->state.lock_fd,
                      bootstrap->snapshots.directory_fd, pipe_fds[1]};
        qsort(keep, 4, sizeof(keep[0]), compare_fd);
        unsigned int first = 3;
        for (size_t i = 0; i < 4; i++)
        {
            if (keep[i] < 3)
                continue;
            if (first < (unsigned int)keep[i])
                (void)close_range(first, (unsigned int)keep[i] - 1, 0);
            first = (unsigned int)keep[i] + 1;
        }
        (void)close_range(first, ~0u, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
            _exit(1);
        char message[256] = "";
        int rc = lcs_config_bootstrap_refresh(bootstrap, cfg, message, sizeof(message));
        if (rc)
        {
            ssize_t sent = write(pipe_fds[1], message, strlen(message));
            (void)sent;
        }
        _exit(rc ? 1 : 0);
    }
    close(pipe_fds[1]);
    bootstrap->worker_pid = child;
    bootstrap->worker_pipe = pipe_fds[0];
    bootstrap->refresh_pending = false;
    return result;
}
