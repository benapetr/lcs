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
int config_peer_receive_readiness(int epoll_fd, int node, uint16_t type,
                                   const void *payload, size_t length);

#endif
