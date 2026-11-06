// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_LIVE_H
#define LCS_CONFIG_LIVE_H

#include "config_reconcile.h"
#include "config_projection.h"
#include "config_owner_network.h"

typedef int (*lcs_config_live_projection_t)(void *context, const lcs_config_value_t *committed,
                                             char *err, size_t err_len);

typedef struct
{
    int epoll_fd;
    lcs_config_live_projection_t project;
    lcs_config_reconcile_step_t owner_barrier;
    void (*owner_revoke)(void *context);
    void *barrier_context;
    uint64_t attempt, revision;
    char digest[LCS_CONFIG_DIGEST_SIZE];
    bool bound, installed, synchronizing;
    bool managed_projection;
    lcs_config_projection_t projection;
    lcs_config_owner_network_t *network; /* Optional concrete instance observations. */
} lcs_config_live_t;

/* Explicit adapter only; no automatic initialization/application is enabled.
 * project must publish the LATEST learned projection asynchronously and idempotently.
 * owner_barrier must establish generation-bound old-owner stop/expiry/fencing
 * evidence; the adapter does not infer that evidence from gossip or a timeout.
 * Both are mandatory. The projection callback receives the latest learned value,
 * which may differ from an older pending application. All callback data is
 * borrowed for that invocation only; copy and bind asynchronous results to its
 * digest. Callbacks obey config_reconcile's nonblocking contract.
 * Runtime tables must reflect recovered last-applied ownership/backend state;
 * this live adapter does not discover ownerless leftover backends after restart.
 * Installation republishes the latest learned projection. Admission enables
 * fresh matching-generation sync with all resource grants fenced; paired
 * cancel/complete callbacks retain fences until the sequencer transfers its hold.
 * Keep adapter, reconciler and daemon runtime alive through terminal shutdown. */
int lcs_config_live_init(lcs_config_live_t *live, int epoll_fd,
                           lcs_config_live_projection_t project,
                           lcs_config_reconcile_step_t owner_barrier, void *context);
/* Production projection provider using the shared coordinator writer. The
 * distributed old-owner barrier remains mandatory and caller-supplied. */
int lcs_config_live_init_with_projection(lcs_config_live_t *live, int epoll_fd,
                                          lcs_config_coordinator_t *coordinator,
                                          lcs_config_bootstrap_t *bootstrap,
                                          lcs_config_reconcile_step_t owner_barrier, void *context);
/* Concrete all-full-member stop exchange, still explicit and startup-gated.
 * Network must borrow this exact runtime's base/catalog, and its transport must
 * process admission/session changes before reconciliation. Local backend state
 * must already be recovered. Receipts remain pinned until all full members
 * confirm exact durable application; restart/catch-up recovery remains gated. */
int lcs_config_live_init_with_owner_network(lcs_config_live_t *live, int epoll_fd,
                                             lcs_config_coordinator_t *coordinator,
                                             lcs_config_bootstrap_t *bootstrap,
                                             lcs_config_owner_network_t *network);
const lcs_config_reconcile_ops_t *lcs_config_live_ops(void);
/* Resource scheduler hook: during a fenced generation handover, retain existing
 * unchanged leases only until their current deadline; never renew without quorum.
 * Returns true when normal lease maintenance must remain paused. */
bool lcs_config_live_maintain_leases(int epoll_fd);
#endif
