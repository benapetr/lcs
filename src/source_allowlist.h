// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_SOURCE_ALLOWLIST_H
#define LCS_SOURCE_ALLOWLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#define LCS_ALLOW_FROM_MAX 255
#define LCS_SOURCE_NETWORK_MAX 16
#define LCS_DEFAULT_ALLOW_FROM "10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"

typedef struct
{
    int family;
    uint8_t address[16];
    unsigned int prefix;
} lcs_source_network_t;

typedef struct
{
    bool disabled;
    size_t count;
    lcs_source_network_t networks[LCS_SOURCE_NETWORK_MAX];
} lcs_source_allowlist_t;

int lcs_source_allowlist_parse(lcs_source_allowlist_t *list, const char *value);
bool lcs_source_allowlist_contains(const lcs_source_allowlist_t *list,
                                   const struct sockaddr *source);

#endif
