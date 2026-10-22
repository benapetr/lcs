// SPDX-License-Identifier: GPL-3.0-or-later

#include "source_allowlist.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

int lcs_source_allowlist_parse(lcs_source_allowlist_t *list, const char *value)
{
    /* Parse atomically: a malformed suffix must not leave an active bypass. */
    lcs_source_allowlist_t candidate = {0};
    char buffer[LCS_ALLOW_FROM_MAX + 1];
    if (!value || !*value || strlen(value) > LCS_ALLOW_FROM_MAX)
        return -1;
    strcpy(buffer, value);
    char *entry = buffer;
    for (;;)
    {
        char *next = strchr(entry, ',');
        if (next)
            *next++ = '\0';
        while (isspace((unsigned char)*entry))
            entry++;
        char *end = entry + strlen(entry);
        while (end > entry && isspace((unsigned char)end[-1]))
            *--end = '\0';
        if (!*entry || candidate.count == LCS_SOURCE_NETWORK_MAX)
            return -1;
        char *prefix = strchr(entry, '/');
        if (!prefix || !prefix[1])
            return -1;
        *prefix++ = '\0';
        for (const char *p = prefix; *p; p++)
            if (!isdigit((unsigned char)*p))
                return -1;
        /* At most three digits, so conversion cannot overflow. */
        if (strlen(prefix) > 3)
            return -1;
        unsigned long bits = strtoul(prefix, NULL, 10);
        lcs_source_network_t *network = &candidate.networks[candidate.count];
        if (inet_pton(AF_INET, entry, network->address) == 1)
        {
            network->family = AF_INET;
            if (bits > 32)
                return -1;
            if (bits == 0)
                candidate.disabled = true;
        } else if (inet_pton(AF_INET6, entry, network->address) == 1)
        {
            network->family = AF_INET6;
            if (bits > 128)
                return -1;
        } else
            return -1;
        network->prefix = (unsigned int)bits;
        candidate.count++;
        if (!next)
            break;
        entry = next;
    }
    *list = candidate;
    return 0;
}

int lcs_source_parse_ip(const char *value, struct sockaddr_storage *address)
{
    memset(address, 0, sizeof(*address));
    struct sockaddr_in *v4 = (struct sockaddr_in *)address;
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)address;
    if (inet_pton(AF_INET, value, &v4->sin_addr) == 1)
        v4->sin_family = AF_INET;
    else if (inet_pton(AF_INET6, value, &v6->sin6_addr) == 1)
        v6->sin6_family = AF_INET6;
    else
        return -1;
    return 0;
}

static const uint8_t *source_ip_bytes(const struct sockaddr *source, int *family)
{
    *family = source->sa_family;
    if (*family == AF_INET)
        return (const uint8_t *)&((const struct sockaddr_in *)source)->sin_addr;
    if (*family == AF_INET6)
    {
        const struct in6_addr *v6 = &((const struct sockaddr_in6 *)source)->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(v6))
        {
            *family = AF_INET;
            return v6->s6_addr + 12;
        }
        return v6->s6_addr;
    }
    return NULL;
}

bool lcs_source_same_ip(const struct sockaddr *a, const struct sockaddr *b)
{
    int a_family, b_family;
    const uint8_t *a_bytes = source_ip_bytes(a, &a_family);
    const uint8_t *b_bytes = source_ip_bytes(b, &b_family);
    return a_bytes && b_bytes && a_family == b_family &&
           memcmp(a_bytes, b_bytes, a_family == AF_INET ? 4 : 16) == 0;
}

bool lcs_source_allowlist_contains(const lcs_source_allowlist_t *list,
                                   const struct sockaddr *source)
{
    int family;
    const uint8_t *address = source_ip_bytes(source, &family);
    if (!address)
        return false;
    if (list->disabled)
        return true;
    for (size_t i = 0; i < list->count; i++)
    {
        const lcs_source_network_t *network = &list->networks[i];
        if (network->family != family)
            continue;
        unsigned int bytes = network->prefix / 8;
        unsigned int bits = network->prefix % 8;
        if (memcmp(address, network->address, bytes) != 0)
            continue;
        if (!bits || ((address[bytes] ^ network->address[bytes]) &
                      (0xffu << (8 - bits))) == 0)
            return true;
    }
    return false;
}
