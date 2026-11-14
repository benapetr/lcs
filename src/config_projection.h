// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_PROJECTION_H
#define LCS_CONFIG_PROJECTION_H

#include "config_coordinator.h"

typedef struct
{
    lcs_config_coordinator_t *coordinator;
    lcs_config_bootstrap_t *bootstrap;
    uint64_t completion;
} lcs_config_projection_t;

/* Borrowed coordinator/bootstrap must live until their shared writer is reaped.
 * No I/O during initialization or step; only the storage child touches files. */
void lcs_config_projection_init(lcs_config_projection_t *projection,
                                  lcs_config_coordinator_t *coordinator, lcs_config_bootstrap_t *bootstrap);
/* Nonblocking projection-provider callback: 0 pending, 1 latest learned value
 * durably published, -1 blocked/recovery. Known file errors may be retried after
 * repair; a cancelled/broken writer requires ordinary coordinator recovery.
 * Never acknowledges a stale request/value or a superseded completion. */
int lcs_config_projection_step(void *context, const lcs_config_value_t *committed, char *err, size_t err_len);
#endif
