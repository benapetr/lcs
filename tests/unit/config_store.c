// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_store.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_sync;
static bool fail_rename;
int __real_fsync(int fd);
int __real_renameat(int olddir, const char *old, int newdir, const char *new);
int __wrap_fsync(int fd)
{
    if (fail_sync && !--fail_sync)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
int __wrap_renameat(int olddir, const char *old, int newdir, const char *new)
{
    if (fail_rename)
    {
        errno = EIO;
        return -1;
    }
    return __real_renameat(olddir, old, newdir, new);
}

static void contents(lcs_config_store_t *store, const char *expected)
{
    char *data = NULL, err[256];
    size_t length = 0;
    assert(lcs_config_store_read(store, "snapshot.json", &data, &length, 64, err, sizeof(err)) == 0);
    assert(length == strlen(expected) && strcmp(data, expected) == 0);
    free(data);
}

int main(void)
{
    char directory[] = "/tmp/lcs-config-store.XXXXXX", err[256];
    assert(mkdtemp(directory));
    lcs_config_store_t store, other;
    assert(lcs_config_store_open(&store, directory, err, sizeof(err)) == 0);
    assert(lcs_config_store_open(&other, directory, err, sizeof(err)) == -1);
    char *data = NULL;
    size_t length = 99;
    assert(lcs_config_store_read(&store, "snapshot.json", &data, &length, 64, err, sizeof(err)) == 1);
    assert(data == NULL && length == 99);
    assert(lcs_config_store_write(&store, "snapshot.json", "old", 3, err, sizeof(err)) == 0);
    contents(&store, "old");
    struct stat st;
    assert(fstatat(store.directory_fd, "snapshot.json", &st, 0) == 0);
    assert((st.st_mode & 0777) == 0600);

    fail_sync = 1; /* File fsync failure: old committed record survives. */
    assert(lcs_config_store_write(&store, "snapshot.json", "new", 3, err, sizeof(err)) == -1);
    contents(&store, "old");
    fail_rename = true;
    assert(lcs_config_store_write(&store, "snapshot.json", "new", 3, err, sizeof(err)) == -1);
    fail_rename = false;
    contents(&store, "old");
    fail_sync = 2; /* Rename succeeded, directory sync failed: outcome unknown. */
    assert(lcs_config_store_write(&store, "snapshot.json", "new", 3, err, sizeof(err)) == -1);
    assert(strstr(err, "outcome uncertain"));
    contents(&store, "new");
    lcs_config_store_close(&store);
    assert(lcs_config_store_open(&store, directory, err, sizeof(err)) == 0);
    contents(&store, "new");

    assert(lcs_config_store_read(&store, "snapshot.json", &data, &length, 2, err, sizeof(err)) == -1);
    assert(lcs_config_store_write(&store, "../escape", "x", 1, err, sizeof(err)) == -1);
    assert(lcs_config_store_write(&store, "lock", "x", 1, err, sizeof(err)) == -1);
    assert(symlinkat("snapshot.json", store.directory_fd, "link") == 0);
    assert(lcs_config_store_read(&store, "link", &data, &length, 64, err, sizeof(err)) == -1);
    assert(lcs_config_store_write(&store, "link", "x", 1, err, sizeof(err)) == -1);
    assert(mkfifoat(store.directory_fd, "fifo", 0600) == 0);
    assert(lcs_config_store_read(&store, "fifo", &data, &length, 64, err, sizeof(err)) == -1);
    assert(lcs_config_store_write(&store, "fifo", "x", 1, err, sizeof(err)) == -1);
    assert(unlinkat(store.directory_fd, "link", 0) == 0);
    assert(unlinkat(store.directory_fd, "fifo", 0) == 0);
    assert(unlinkat(store.directory_fd, "snapshot.json", 0) == 0);
    assert(unlinkat(store.directory_fd, "lock", 0) == 0);
    lcs_config_store_close(&store);
    assert(chmod(directory, 0777) == 0);
    assert(lcs_config_store_open(&store, directory, err, sizeof(err)) == -1);
    assert(rmdir(directory) == 0);
    puts("configuration store unit tests passed");
    return 0;
}
