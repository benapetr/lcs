// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_PEER_H
#define LCS_CONFIG_PEER_H

#include "config_status_protocol.h"
#include "config_agreement.h"

/* Diagnostic metadata, freeze and explicit readiness have distinct message types.
 * Metadata never authorizes activation or resource mutations. */
int config_peer_refresh(void);
void config_peer_process(int epoll_fd);
int config_peer_receive(int node, const void *payload, size_t length);
int config_peer_view(lcs_config_status_view_t *view);
/* Readiness exchange after durable local initialization and serialized INI
 * reload. The scheduler resumes this automatically after an existing freeze.
 * A completed in-memory round still requires the shared barrier storage worker before configuration votes. */
int config_peer_readiness_start(void);
/* Explicit freeze collection only, after the local freeze is durable. Requires
 * every current member (including quorum-only) to negotiate freeze support.
 * Completion never automatically initializes journals or changes authority. */
int config_peer_freeze_start(void);
/* Explicit per-member initialization workflow: collect all current durable
 * freezes (including quorum-only) then promote through the shared writer.
 * An existing collection may be used. The scheduler resumes existing freezes;
 * this does not create the first freeze or activate JSON. */
int config_peer_initialization_start(void);
/* Before listeners/workers only: restore a recovered FROZEN or validated
 * mandatory-history revision-zero PREPARED baseline and its coordinator,
 * including promises/reservations/accepted activation with a valid barrier,
 * while retaining resource/reload fences. Cleanup uses the retained baseline. This
 * restore itself does no initialization or authority switching. */
int config_peer_resume_frozen(char *err, size_t err_len);
int config_peer_receive_freeze(int epoll_fd, int node, uint16_t type,
    const void *payload, size_t length);
/* Internal entry point for one whole-snapshot proposal using currently admitted
 * wire-6 instances. Requires durable readiness and serialized INI reload.
 * Completion is available from config_transactions; it does not apply resources. */
int config_peer_propose(uint64_t id, const lcs_config_value_t *requested);
int config_peer_propose_client(uint64_t id, const lcs_config_value_t *requested, const char *client_digest);
/* Shutdown never frees/reopens storage while a cancelled writer is alive.
 * Call stop once, poll shutdown_process, then check stopped before closing the
 * runtime/storage. A false result at process exit means retain those handles. */
void config_peer_stop(void);
void config_peer_shutdown_process(void);
bool config_peer_stopped(void);
bool config_peer_configuration_vote_ready(void);
int config_peer_receive_readiness(int epoll_fd, int node, uint16_t type,
                                   const void *payload, size_t length);
/* Owner exchange never starts cleanup or seals a receipt on a query. The live
 * manager initializes the explicit local adapter separately after startup safety. */
int config_peer_receive_owner(int epoll_fd, int node, const void *payload, size_t length);
/* Initialize/validate the borrowed owner transport against the current immutable
 * static base. Does not start a collection or authorize backend application. */
int config_peer_owner_prepare(void);
int config_peer_receive_transaction(int epoll_fd, int node, uint16_t type,
                                     const void *payload, size_t length);

#endif
