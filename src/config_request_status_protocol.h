// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_REQUEST_STATUS_PROTOCOL_H
#define LCS_CONFIG_REQUEST_STATUS_PROTOCOL_H
#include "config_requests.h"
#include "protocol.h"

#define LCS_CONFIG_REQUEST_STATUS_FORMAT 1
#define LCS_CONFIG_REQUEST_STATUS_SIZE 206u
int lcs_config_request_id_encode(const char *id, void *payload, size_t cap, size_t *length);
int lcs_config_request_id_decode(const void *payload, size_t length, char id[LCS_CONFIG_REQUEST_MAX + 1]);
int lcs_config_request_view_encode(const lcs_config_request_view_t *view, void *payload, size_t cap, size_t *length);
int lcs_config_request_view_decode(const void *payload, size_t length, lcs_config_request_view_t *view);
#endif
