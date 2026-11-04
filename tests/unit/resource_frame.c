// SPDX-License-Identifier: GPL-3.0-or-later
#include "resource_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    lcs_resource_generation_t generation = { .revision = 7 }, decoded;
    memset(generation.membership, 'a', 64); memset(generation.digest, 'b', 64);
    unsigned char payload[LCS_MAX_FRAME + 1];
    size_t length, body_length;
    const void *body;
    uint16_t type;
    assert(!lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_LEASE_ACK, 3, payload, sizeof(payload), &length));
    assert(length == LCS_RESOURCE_FRAME_PREFIX);
    memcpy(payload + length, "abc", 3); length += 3;
    assert(!lcs_resource_frame_decode(payload, length, 100, 101, &decoded, &type, &body, &body_length));
    assert(lcs_resource_generation_equal(&generation, &decoded) && type == LCS_MSG_LEASE_ACK);
    assert(body_length == 3 && !memcmp(body, "abc", 3));
    for (size_t i = 0; i < LCS_RESOURCE_FRAME_PREFIX; i++)
        assert(lcs_resource_frame_decode(payload, i, 100, 101, &decoded, &type, &body, &body_length) < 0);
    assert(lcs_resource_frame_decode(payload, length, 102, 101, &decoded, &type, &body, &body_length) < 0);
    assert(lcs_resource_frame_decode(payload, length, 100, 102, &decoded, &type, &body, &body_length) < 0);
    payload[92] = 'A'; /* Membership field must be canonical lowercase hexadecimal. */
    assert(lcs_resource_frame_decode(payload, length, 100, 101, &decoded, &type, &body, &body_length) < 0);
    assert(!lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_HEARTBEAT, 0, payload, sizeof(payload), &length));
    payload[LCS_RESOURCE_FRAME_PREFIX - 1] = 'x';
    assert(lcs_resource_frame_decode(payload, length, 100, 101, &decoded, &type, &body, &body_length) < 0);
    assert(lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_CONFIG_TX_CONTROL, 0, payload, sizeof(payload), &length) < 0);
    assert(lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_RESOURCE_FRAME, 0, payload, sizeof(payload), &length) < 0);
    assert(!lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_STATE_SYNC_REQ,
        LCS_MAX_FRAME - LCS_RESOURCE_FRAME_PREFIX, payload, sizeof(payload), &length));
    assert(lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_STATE_SYNC_REQ,
        LCS_MAX_FRAME - LCS_RESOURCE_FRAME_PREFIX + 1, payload, sizeof(payload), &length) < 0);
    assert(lcs_resource_frame_decode(payload, sizeof(payload), 100, 101, &decoded, &type, &body, &body_length) < 0);
    generation.revision = 0;
    assert(lcs_resource_frame_prefix(&generation, 100, 101, LCS_MSG_LEASE_REQ, 0, payload, sizeof(payload), &length) < 0);
    puts("resource generation frame codec unit tests passed");
    return 0;
}
