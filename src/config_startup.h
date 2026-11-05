// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_STARTUP_H
#define LCS_CONFIG_STARTUP_H
#include "config_owner_network.h"
#include <sys/types.h>

typedef enum
{
    LCS_CONFIG_STARTUP_IDLE = 0,
    LCS_CONFIG_STARTUP_NEXT,
    LCS_CONFIG_STARTUP_PRE_STOP,
    LCS_CONFIG_STARTUP_BACKEND,
    LCS_CONFIG_STARTUP_POST_STOP,
    LCS_CONFIG_STARTUP_DONE,
    LCS_CONFIG_STARTUP_BLOCKED,
    LCS_CONFIG_STARTUP_RECOVERY,
    LCS_CONFIG_STARTUP_STOPPED,
} lcs_config_startup_phase_t;

typedef struct
{
    const lcs_config_runtime_t *runtime;
    lcs_config_t *config; /* Owned last-applied definitions, never INI/pending. */
    lcs_config_value_t applied, pending;
    uint64_t instance, deadline_ms;
    uint32_t completed;
    size_t index;
    pid_t pid;
    lcs_config_startup_phase_t phase;
    bool service_worker, hook_worker, killed, closing, invalid, pre_failed, quorum_only;
    char error[256];
} lcs_config_startup_t;

/* Explicit recovery runner only, before normal backend/lease work is allowed.
 * Zero-initialize a fresh object, or reuse only after close. Caller keeps all backend activity and voting fenced and
 * retains runtime/store until close succeeds. Requires a recovered frozen base,
 * valid activation barrier and catalog. No file I/O, ownership/epoch fabrication,
 * journal/catalog writes, generation admission or automatic daemon wiring here. */
int lcs_config_startup_begin(lcs_config_startup_t *startup, const lcs_config_runtime_t *runtime,
                               uint64_t instance, char *err, size_t err_len);
/* Nonblocking cleanup of ALL last-applied backends, dependents first. Uses old
 * pre/post-stop hooks and existing VIP/systemd workers. Pre-stop failure still
 * attempts backend removal, but blocks final success. Failed cleanup requires
 * explicit retry; cancellation/changed catalog cannot count as success. */
int lcs_config_startup_process(lcs_config_startup_t *startup, uint64_t now_ms);
int lcs_config_startup_retry(lcs_config_startup_t *startup);
bool lcs_config_startup_verified(const lcs_config_startup_t *startup,
                                   const lcs_config_runtime_t *runtime, uint64_t instance);
/* Recreate only a pending stop scope after actual startup cleanup, using the
 * NEW daemon instance and round. Cannot reconstruct completed receipt history
 * from an applied revision; lease/grant recovery and admission remain separate. */
int lcs_config_startup_restore_pending(lcs_config_startup_t *startup,
                                       lcs_config_owner_network_t *network, uint64_t now_ms);
/* Terminal: request cancellation but retain pid/snapshots until process reaps.
 * close returns -1 while a worker exists or before DONE/STOPPED/RECOVERY/BLOCKED. */
void lcs_config_startup_stop(lcs_config_startup_t *startup);
int lcs_config_startup_close(lcs_config_startup_t *startup);
#endif
