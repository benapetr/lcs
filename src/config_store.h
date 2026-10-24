// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_STORE_H
#define LCS_CONFIG_STORE_H

#include <stddef.h>

/* A process exclusively owns one state directory. Files are addressed relative
 * to its open directory descriptor; symlink files and nonregular files are
 * rejected. This is a local durability primitive, not distributed agreement. */
typedef struct
{
    int directory_fd;
    int lock_fd;
} lcs_config_store_t;

int lcs_config_store_open(lcs_config_store_t *store, const char *directory,
                          char *err, size_t err_len);
void lcs_config_store_close(lcs_config_store_t *store);
/* Returns 1 only for ENOENT, 0 for success, -1 for other failures. */
int lcs_config_store_read(lcs_config_store_t *store, const char *name,
                          char **data, size_t *length, size_t maximum,
                          char *err, size_t err_len);
/* fsync(temp), rename, fsync(directory). On failure after rename the new file
 * may already be visible: callers must recover, never assume rollback. */
int lcs_config_store_write(lcs_config_store_t *store, const char *name,
                           const void *data, size_t length,
                           char *err, size_t err_len);

#endif
