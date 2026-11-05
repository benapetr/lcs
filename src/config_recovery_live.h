// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_RECOVERY_LIVE_H
#define LCS_CONFIG_RECOVERY_LIVE_H
#include "config_recovery.h"

typedef struct
{
    lcs_config_recovery_t *recovery;
    int epoll_fd;
    uint64_t attempt;
    char digest[LCS_CONFIG_DIGEST_SIZE];
    bool prepared;
} lcs_config_recovery_live_t;

/* Zero-initialize the adapter and an unstarted controller.
 * Explicit pre-listener bootstrap for an already activated schema-2 catalog.
 * Requires pristine daemon resource/grant tables, no sessions/ordinary workers,
 * valid recovered authority and a new adapter. Install the exact last-applied
 * definitions (never INI/pending), fence every resource and publish an unadmitted
 * wire-6 generation. Does not initialize a coordinator, clean a backend, or
 * authorize leases/votes. Revision-zero activation remains a separate path. */
int lcs_config_recovery_live_prepare(lcs_config_recovery_live_t *live,
                                       lcs_config_recovery_t *recovery, int epoll_fd,
                                       char *err, size_t err_len);
/* Mandatory recovery-controller install callback. Requires verified physical
 * cleanup and the controller's exact immutable attempt/plan. Normalize observed
 * ownership to stopped, retain safety epochs/private grants and runtime policies
 * only for matching incarnations/backends, and reset replacements/new resources.
 * Cancel old indexed traffic before remapping. No admission or backend activation;
 * closing generation sessions may require a fresh explicit owner-proof retry. */
int lcs_config_recovery_live_install(void *context, const lcs_config_reconcile_view_t *view,
                                       char *err, size_t err_len);
#endif
