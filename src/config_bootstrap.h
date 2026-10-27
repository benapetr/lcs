// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_BOOTSTRAP_H
#define LCS_CONFIG_BOOTSTRAP_H

#include "config_store.h"
#include "resource_config.h"
#include <sys/types.h>

typedef struct
{
    lcs_config_store_t state;
    lcs_config_store_t snapshots;
    char snapshot_name[LCS_PATH_MAX + 1];
    pid_t worker_pid;
    int worker_pipe;
    bool refresh_pending;
    bool prepared;
} lcs_config_bootstrap_t;

/* Prepares revision-zero JSON only. INI remains authoritative; this does not
 * advertise support for distributed activation or enable resource mutations. */
int lcs_config_bootstrap_open(lcs_config_bootstrap_t *bootstrap,
                              const char *config_path, const lcs_config_t *cfg,
                              char *err, size_t err_len);
int lcs_config_bootstrap_refresh(lcs_config_bootstrap_t *bootstrap,
                                 const lcs_config_t *cfg,
                                 char *err, size_t err_len);
void lcs_config_bootstrap_close(lcs_config_bootstrap_t *bootstrap);
void lcs_config_bootstrap_request(lcs_config_bootstrap_t *bootstrap);
/* Nonblocking scheduler work. Returns 1 for a completed refresh, -1 for a
 * failure (with error text), 0 when no completion is available. */
int lcs_config_bootstrap_process(lcs_config_bootstrap_t *bootstrap,
                                 const lcs_config_t *cfg,
                                 char *err, size_t err_len);

#endif
