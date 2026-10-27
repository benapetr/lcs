// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_PEER_H
#define LCS_CONFIG_PEER_H

#include "config_status_protocol.h"

/* Only diagnostic prepared-snapshot metadata is exchanged at this stage.
 * Fresh advertisements do not authorize activation or resource mutations. */
int config_peer_refresh(void);
void config_peer_process(int epoll_fd);
int config_peer_receive(int node, const void *payload, size_t length);
int config_peer_view(lcs_config_status_view_t *view);

#endif
