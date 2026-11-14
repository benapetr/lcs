// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_request_status_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char payload[512], original[512]; size_t length;
    char id[LCS_CONFIG_REQUEST_MAX + 1];
    assert(!lcs_config_request_id_encode("request-a.1", payload, sizeof(payload), &length));
    assert(length == 66 && !lcs_config_request_id_decode(payload, length, id) && !strcmp(id, "request-a.1"));
    for (size_t i = 0; i < length; i++) assert(lcs_config_request_id_decode(payload, i, id) < 0);
    assert(lcs_config_request_id_decode(payload, length + 1, id) < 0);
    payload[65] = 1; assert(lcs_config_request_id_decode(payload, length, id) < 0);
    assert(lcs_config_request_id_encode("bad/id", payload, sizeof(payload), &length) < 0);
    assert(lcs_config_request_id_encode("", payload, sizeof(payload), &length) < 0);
    char long_id[65]; memset(long_id, 'a', 64); long_id[64] = 0;
    assert(lcs_config_request_id_encode(long_id, payload, sizeof(payload), &length) < 0);
    long_id[63] = 0;
    assert(!lcs_config_request_id_encode(long_id, payload, sizeof(payload), &length));
    assert(!lcs_config_request_id_decode(payload, length, id) && !strcmp(long_id, id));
    lcs_config_request_view_t view = {0}, decoded;
    strcpy(view.id, "request-a");
    for (unsigned history = 0; history <= LCS_CONFIG_HISTORY_RECOVERY; history++)
    {
        view.history = (uint8_t)history;
        assert(!lcs_config_request_view_encode(&view, payload, sizeof(payload), &length));
        assert(length == LCS_CONFIG_REQUEST_STATUS_SIZE);
        assert(!lcs_config_request_view_decode(payload, length, &decoded));
        assert(decoded.history == history && !decoded.state && !decoded.base_revision && !*decoded.value_digest);
    }
    view.history = LCS_CONFIG_HISTORY_AVAILABLE;
    view.base_revision = LCS_RESOURCE_REVISION_MAX - 1;
    memset(view.base_digest, 'a', 64); memset(view.value_digest, 'b', 64);
    for (unsigned state = LCS_CONFIG_REQUEST_RESERVED; state <= LCS_CONFIG_REQUEST_APPLIED; state++)
    {
        view.state = (uint8_t)state;
        assert(!lcs_config_request_view_encode(&view, payload, sizeof(payload), &length));
        assert(!lcs_config_request_view_decode(payload, length, &decoded));
        assert(decoded.state == state && decoded.base_revision == view.base_revision && !strcmp(decoded.value_digest, view.value_digest));
        for (size_t i = 0; i < length; i++) assert(lcs_config_request_view_decode(payload, i, &decoded) < 0);
    }
    assert(lcs_config_request_view_encode(&view, payload, length - 1, &length) < 0);
    assert(!lcs_config_request_view_encode(&view, original, sizeof(original), &length));
    memcpy(payload, original, length); payload[1] = 1;
    assert(lcs_config_request_view_decode(payload, length, &decoded) < 0); /* Old format cannot claim admission. */
    for (unsigned mode = 0; mode < 9; mode++)
    {
        memcpy(payload, original, length); size_t size = length;
        if (mode == 0) payload[1] = 3;
        if (mode == 1) payload[2] = 3;
        if (mode == 2) payload[3] = 4;
        if (mode == 3) payload[2] = LCS_CONFIG_HISTORY_UNAVAILABLE;
        if (mode == 4) payload[3] = LCS_CONFIG_REQUEST_UNKNOWN;
        if (mode == 5) payload[67] = 1; /* Noncanonical name padding. */
        if (mode == 6) payload[76] = 'A'; /* Digests use lowercase hex. */
        if (mode == 7) payload[140] = 1; /* Unterminated digest. */
        if (mode == 8) size++;
        memset(&decoded, 0x5a, sizeof(decoded)); lcs_config_request_view_t before = decoded;
        assert(lcs_config_request_view_decode(payload, size, &decoded) < 0 && !memcmp(&decoded, &before, sizeof(decoded)));
    }
    view.base_revision = LCS_RESOURCE_REVISION_MAX;
    assert(lcs_config_request_view_encode(&view, payload, sizeof(payload), &length) < 0);
    puts("local request-status codec validation unit tests passed");
    return 0;
}
