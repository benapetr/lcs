// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_store.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int error(char *err, size_t len, const char *operation)
{
    int saved = errno;
    if (err && len)
        snprintf(err, len, "configuration store %s: %s", operation, strerror(saved));
    errno = saved;
    return -1;
}

static int valid_name(const char *name)
{
    return name && *name && strlen(name) <= NAME_MAX &&
           name[0] != '.' && !strchr(name, '/') && strcmp(name, "lock") != 0;
}

static int sync_parent(const char *directory)
{
    char *path = strdup(directory);
    if (!path)
        return -1;
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
    char *slash = strrchr(path, '/');
    const char *parent = ".";
    if (slash)
    {
        if (slash == path)
            slash[1] = '\0';
        else
            *slash = '\0';
        parent = path;
    }
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(path);
    if (fd < 0)
        return -1;
    int rc = fsync(fd), saved = errno;
    close(fd);
    errno = saved;
    return rc;
}

int lcs_config_store_open(lcs_config_store_t *store, const char *directory,
                          char *err, size_t err_len)
{
    store->directory_fd = store->lock_fd = -1;
    int created = mkdir(directory, 0700) == 0;
    if (!created && errno != EEXIST)
        return error(err, err_len, "create directory");
    store->directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (store->directory_fd < 0)
        return error(err, err_len, "open directory");
    struct stat st;
    if (fstat(store->directory_fd, &st))
        goto fail;
    if (st.st_uid != geteuid() || (st.st_mode & 0022))
    {
        errno = EPERM;
        goto fail;
    }
    store->lock_fd = openat(store->directory_fd, "lock",
                           O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (store->lock_fd < 0 || fstat(store->lock_fd, &st))
        goto fail;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid())
    {
        errno = EPERM;
        goto fail;
    }
    if (flock(store->lock_fd, LOCK_EX | LOCK_NB))
        goto fail;
    /* Also retry after a previous failed initialization left the directory
     * present without confirming durability of its parent entry. */
    if (sync_parent(directory))
        goto fail;
    return 0;
fail:
    error(err, err_len, "open/lock private state directory");
    lcs_config_store_close(store);
    return -1;
}

void lcs_config_store_close(lcs_config_store_t *store)
{
    int saved = errno;
    if (store->lock_fd >= 0)
        close(store->lock_fd);
    if (store->directory_fd >= 0)
        close(store->directory_fd);
    store->lock_fd = store->directory_fd = -1;
    errno = saved;
}

int lcs_config_store_read(lcs_config_store_t *store, const char *name,
                          char **data, size_t *length, size_t maximum,
                          char *err, size_t err_len)
{
    if (!valid_name(name) || !maximum || maximum == SIZE_MAX)
    {
        errno = EINVAL;
        return error(err, err_len, "invalid read parameters");
    }
    int fd = openat(store->directory_fd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? 1 : error(err, err_len, "open file");
    struct stat st;
    char *buffer = NULL;
    if (fstat(fd, &st))
        goto fail;
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() || (st.st_mode & 0022) ||
        st.st_size < 0 || (uintmax_t)st.st_size > maximum)
    {
        errno = EINVAL;
        goto fail;
    }
    buffer = malloc(maximum + 1);
    if (!buffer)
        goto fail;
    size_t used = 0;
    for (;;)
    {
        ssize_t n = read(fd, buffer + used, maximum + 1 - used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto fail;
        if (!n)
            break;
        used += (size_t)n;
        if (used > maximum)
        {
            errno = EFBIG;
            goto fail;
        }
    }
    buffer[used] = '\0';
    if (close(fd))
    {
        free(buffer);
        return error(err, err_len, "close file");
    }
    *data = buffer;
    *length = used;
    return 0;
fail:
    error(err, err_len, "read bounded regular file");
    free(buffer);
    close(fd);
    return -1;
}

int lcs_config_store_write(lcs_config_store_t *store, const char *name,
                           const void *data, size_t length,
                           char *err, size_t err_len)
{
    if (!valid_name(name) || (!data && length))
    {
        errno = EINVAL;
        return error(err, err_len, "invalid write parameters");
    }
    /* Exclusive directory locking serializes writers. A leftover temporary
     * file is never interpreted as a committed record after a crash. */
    char temporary[NAME_MAX + 1];
    if (snprintf(temporary, sizeof(temporary), ".%s.tmp", name) >= (int)sizeof(temporary))
    {
        errno = ENAMETOOLONG;
        return error(err, err_len, "temporary filename");
    }
    if (unlinkat(store->directory_fd, temporary, 0) && errno != ENOENT)
        return error(err, err_len, "remove abandoned temporary file");
    struct stat st;
    int exists = fstatat(store->directory_fd, name, &st, AT_SYMLINK_NOFOLLOW);
    if ((exists && errno != ENOENT) ||
        (!exists && (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_uid != geteuid() || (st.st_mode & 0022))))
    {
        if (!exists)
            errno = EINVAL;
        return error(err, err_len, "check replacement target");
    }
    int fd = openat(store->directory_fd, temporary,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return error(err, err_len, "create temporary file");
    size_t used = 0;
    while (used < length)
    {
        size_t chunk = length - used;
        if (chunk > 1024u * 1024u)
            chunk = 1024u * 1024u;
        ssize_t n = write(fd, (const char *)data + used, chunk);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            if (!n)
                errno = EIO;
            goto fail;
        }
        used += (size_t)n;
    }
    if (fsync(fd))
        goto fail;
    if (close(fd))
    {
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (renameat(store->directory_fd, temporary, store->directory_fd, name))
        goto fail;
    if (fsync(store->directory_fd))
        return error(err, err_len, "sync directory after replacement (outcome uncertain)");
    return 0;
fail:
    error(err, err_len, "write atomic record");
    if (fd >= 0)
        close(fd);
    unlinkat(store->directory_fd, temporary, 0);
    return -1;
}
