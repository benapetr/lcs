// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_FREEZE_PROTOCOL_H
#define LCS_CONFIG_FREEZE_PROTOCOL_H
#include "config_initialization.h"

#define LCS_CONFIG_FREEZE_FORMAT 1
#define LCS_CONFIG_FREEZE_SIZE 158u
enum { LCS_FREEZE_QUERY = 1, LCS_FREEZE_REPLY = 2 };

/* Query status is zero; responder_instance explicitly targets the admitted
 * destination process. Replies retain the tuple and use FROZEN/BUSY/MISMATCH/LOCKED. LOCKED proves the durable reload
 * lock only; it is not a drained receipt or initialization authorization.
 * This is separate from advisory status and post-initialization readiness. */
typedef struct
{
    uint8_t kind;
    lcs_config_initialization_receipt_t receipt;
} lcs_config_freeze_message_t;

int lcs_config_freeze_encode(const lcs_config_freeze_message_t *message,
    void *payload, size_t capacity, size_t *length);
int lcs_config_freeze_decode(const void *payload, size_t length,
    lcs_config_freeze_message_t *message);
#endif
