// SPDX-License-Identifier: GPL-3.0-or-later

#include "protocol.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    uint16_t effective = 0;
    assert(lcs_protocol_negotiate(5, 5, 5, 5, &effective) == 0);
    assert(effective == 5);
    assert(lcs_protocol_negotiate(5, 7, 5, 6, &effective) == 0);
    assert(effective == 6);
    assert(lcs_protocol_negotiate(5, 6, 6, 8, &effective) == 0);
    assert(effective == 6);
    assert(lcs_protocol_negotiate(6, 8, 4, 5, &effective) != 0);
    assert(lcs_protocol_negotiate(5, 6, 7, 8, &effective) != 0);
    assert(lcs_protocol_negotiate(7, 6, 5, 8, &effective) != 0);
    assert(lcs_protocol_negotiate(5, 8, 7, 6, &effective) != 0);
    assert(lcs_protocol_negotiate(5, 8, 5, 8, NULL) != 0);
    assert(strcmp(lcs_peer_protocol_release(5), "1.1.0") == 0);
    assert(lcs_peer_protocol_release(6) == NULL);
    return 0;
}
