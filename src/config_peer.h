// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_PEER_H
#define LCS_CONFIG_PEER_H

#include "config_status_protocol.h"

/* Diagnostic metadata and explicit readiness have distinct message types.
 * Metadata never authorizes activation or resource mutations. */
int config_peer_refresh(void);
void config_peer_process(int epoll_fd);
int config_peer_receive(int node, const void *payload, size_t length);
int config_peer_view(lcs_config_status_view_t *view);
/* Explicit exchange only: automatic initialization/authority switching is not
 * enabled. The future activation coordinator calls start only after durable
 * local initialization and serializing INI reload. A completed in-memory round
 * still requires the shared barrier storage worker before configuration votes. */
int config_peer_readiness_start(void);
/* Shutdown never frees/reopens storage while a cancelled writer is alive.
 * Call stop once, poll shutdown_process, then check stopped before closing the
 * runtime/storage. A false result at process exit means retain those handles. */
void config_peer_stop(void);
void config_peer_shutdown_process(void);
bool config_peer_stopped(void);
bool config_peer_configuration_vote_ready(void);
int config_peer_receive_readiness(int epoll_fd, int node, uint16_t type,
                                   const void *payload, size_t length);

#endif
