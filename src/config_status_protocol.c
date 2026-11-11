// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_status_protocol.h"
#include "util.h"

#include <string.h>

static bool digest_valid(const char digest[LCS_CONFIG_STATUS_DIGEST_SIZE])
{
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f')))
            return false;
    return digest[64] == 0;
}

static bool status_valid(const lcs_config_status_t *status)
{
    return status->serial && status->prepared <= 1 &&
        digest_valid(status->membership) && digest_valid(status->baseline);
}

static int put_status(lcs_buf_writer_t *w, const lcs_config_status_t *status)
{
    if (!status_valid(status))
        return -1;
    return lcs_buf_put_u64(w, status->serial) || lcs_buf_put_u8(w, status->prepared) ||
        lcs_buf_put_fixed_string(w, status->membership, LCS_CONFIG_STATUS_DIGEST_SIZE) ||
        lcs_buf_put_fixed_string(w, status->baseline, LCS_CONFIG_STATUS_DIGEST_SIZE) ? -1 : 0;
}

static int get_string(lcs_buf_reader_t *r, char *string, size_t width)
{
    if (r->off > r->len || width > r->len - r->off)
        return -1;
    const unsigned char *start = r->data + r->off;
    const unsigned char *end = memchr(start, 0, width);
    if (!end)
        return -1;
    for (const unsigned char *p = end; p < start + width; p++)
        if (*p)
            return -1;
    return lcs_buf_get_fixed_string(r, string, width, width);
}

static int get_status(lcs_buf_reader_t *r, lcs_config_status_t *status)
{
    if (lcs_buf_get_u64(r, &status->serial) || lcs_buf_get_u8(r, &status->prepared) ||
        get_string(r, status->membership, sizeof(status->membership)) ||
        get_string(r, status->baseline, sizeof(status->baseline)) || !status_valid(status))
        return -1;
    return 0;
}

bool lcs_config_status_equal(const lcs_config_status_t *a, const lcs_config_status_t *b)
{
    return a->serial == b->serial && a->prepared == b->prepared &&
        !strcmp(a->membership, b->membership) && !strcmp(a->baseline, b->baseline);
}

int lcs_config_status_encode(const lcs_config_status_t *status, void *payload, size_t cap, size_t *length)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u16(&w, LCS_CONFIG_STATUS_FORMAT) || put_status(&w, status))
        return -1;
    *length = w.len;
    return 0;
}

int lcs_config_status_decode(const void *payload, size_t length, lcs_config_status_t *status)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, length);
    uint16_t format;
    lcs_config_status_t next = {0};
    if (lcs_buf_get_u16(&r, &format) || format != LCS_CONFIG_STATUS_FORMAT ||
        get_status(&r, &next) || r.off != r.len)
        return -1;
    *status = next;
    return 0;
}

static bool view_valid(const lcs_config_status_view_t *view)
{
    if (view->authority == LCS_CONFIG_AUTHORITY_JSON)
    {
        if (view->phase < LCS_CONFIG_DIAG_RECOVERING || view->phase > LCS_CONFIG_DIAG_STOPPING ||
            view->dynamic_commands > 1 || (view->dynamic_commands && !view->admitted) ||
            view->admitted > 1 || (view->admitted != (view->phase == LCS_CONFIG_DIAG_ACTIVE)) ||
            !view->applied_revision || view->applied_revision > view->committed_revision ||
            view->committed_revision > INT64_MAX || !digest_valid(view->committed_digest) ||
            !digest_valid(view->applied_digest) ||
            (view->applied_revision == view->committed_revision && strcmp(view->applied_digest, view->committed_digest)) ||
            (view->pending_revision && (view->pending_revision <= view->applied_revision ||
                view->pending_revision > view->committed_revision || !digest_valid(view->pending_digest))) ||
            (!view->pending_revision && *view->pending_digest) ||
            (view->pending_revision == view->committed_revision && view->pending_revision &&
                strcmp(view->pending_digest, view->committed_digest)) ||
            ((view->phase == LCS_CONFIG_DIAG_ACTIVE || view->phase == LCS_CONFIG_DIAG_ADMITTING) &&
                (view->pending_revision || view->applied_revision != view->committed_revision)) ||
            !memchr(view->error, 0, sizeof(view->error))) return false;
    }
    else if (view->authority != LCS_CONFIG_AUTHORITY_INI || view->phase || view->admitted || view->dynamic_commands ||
        view->committed_revision || view->applied_revision || view->pending_revision ||
        *view->committed_digest || *view->applied_digest || *view->pending_digest || *view->error) return false;
    if (!view->count || view->count > LCS_MAX_NODES || view->self >= view->count)
        return false;
    for (size_t i = 0; i < view->count; i++)
    {
        const lcs_config_status_node_t *node = &view->nodes[i];
        if (!memchr(node->name, 0, sizeof(node->name)) || !lcs_valid_name(node->name) ||
            (node->role != LCS_NODE_FULL && node->role != LCS_NODE_QUORUM_ONLY) ||
            node->connected > 1 || node->known > 1 ||
            (!node->connected && (node->known || node->protocol)) ||
            (node->connected && node->protocol < LCS_PEER_PROTO_MIN_VERSION) ||
            (node->known && (node->protocol < LCS_PROTO_FEATURE_CONFIG_STATUS || !status_valid(&node->status))))
            return false;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(node->name, view->nodes[j].name))
                return false;
    }
    return view->nodes[view->self].connected && view->nodes[view->self].known;
}

int lcs_config_status_view_encode(const lcs_config_status_view_t *view, void *payload, size_t cap, size_t *length)
{
    if (!view_valid(view))
        return -1;
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    uint16_t format = view->dynamic_commands ? LCS_CONFIG_STATUS_COMMANDS_FORMAT :
        view->authority == LCS_CONFIG_AUTHORITY_JSON ? LCS_CONFIG_STATUS_VIEW_FORMAT : LCS_CONFIG_STATUS_FORMAT;
    if (lcs_buf_put_u16(&w, format) || lcs_buf_put_u16(&w, view->count) || lcs_buf_put_u16(&w, view->self))
        return -1;
    for (size_t i = 0; i < view->count; i++)
    {
        const lcs_config_status_node_t *node = &view->nodes[i];
        if (lcs_buf_put_fixed_string(&w, node->name, sizeof(node->name)) ||
            lcs_buf_put_u16(&w, node->role) || lcs_buf_put_u16(&w, node->protocol) ||
            lcs_buf_put_u8(&w, node->connected) || lcs_buf_put_u8(&w, node->known) ||
            (node->known && put_status(&w, &node->status)))
            return -1;
    }
    if (format >= LCS_CONFIG_STATUS_VIEW_FORMAT &&
        (lcs_buf_put_u8(&w, view->authority) || lcs_buf_put_u8(&w, view->admitted) ||
         lcs_buf_put_u16(&w, view->phase) || lcs_buf_put_u64(&w, view->committed_revision) ||
         lcs_buf_put_u64(&w, view->applied_revision) || lcs_buf_put_u64(&w, view->pending_revision) ||
         lcs_buf_put_fixed_string(&w, view->committed_digest, sizeof(view->committed_digest)) ||
         lcs_buf_put_fixed_string(&w, view->applied_digest, sizeof(view->applied_digest)) ||
         lcs_buf_put_fixed_string(&w, view->pending_digest, sizeof(view->pending_digest)) ||
         lcs_buf_put_fixed_string(&w, view->error, sizeof(view->error)))) return -1;
    if (format == LCS_CONFIG_STATUS_COMMANDS_FORMAT && lcs_buf_put_u8(&w, view->dynamic_commands)) return -1;
    *length = w.len;
    return 0;
}

int lcs_config_status_view_decode(const void *payload, size_t length, lcs_config_status_view_t *view)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, length);
    lcs_config_status_view_t next = {0};
    uint16_t format;
    if (lcs_buf_get_u16(&r, &format) || (format != LCS_CONFIG_STATUS_FORMAT && format != LCS_CONFIG_STATUS_VIEW_FORMAT && format != LCS_CONFIG_STATUS_COMMANDS_FORMAT) ||
        lcs_buf_get_u16(&r, &next.count) || lcs_buf_get_u16(&r, &next.self) ||
        !next.count || next.count > LCS_MAX_NODES || next.self >= next.count)
        return -1;
    for (size_t i = 0; i < next.count; i++)
    {
        lcs_config_status_node_t *node = &next.nodes[i];
        if (get_string(&r, node->name, sizeof(node->name)) ||
            lcs_buf_get_u16(&r, &node->role) || lcs_buf_get_u16(&r, &node->protocol) ||
            lcs_buf_get_u8(&r, &node->connected) || lcs_buf_get_u8(&r, &node->known) ||
            node->known > 1 || (node->known && get_status(&r, &node->status)))
            return -1;
    }
    if (format >= LCS_CONFIG_STATUS_VIEW_FORMAT &&
        (lcs_buf_get_u8(&r, &next.authority) || next.authority != LCS_CONFIG_AUTHORITY_JSON ||
         lcs_buf_get_u8(&r, &next.admitted) || lcs_buf_get_u16(&r, &next.phase) ||
         lcs_buf_get_u64(&r, &next.committed_revision) || lcs_buf_get_u64(&r, &next.applied_revision) ||
         lcs_buf_get_u64(&r, &next.pending_revision) ||
         get_string(&r, next.committed_digest, sizeof(next.committed_digest)) ||
         get_string(&r, next.applied_digest, sizeof(next.applied_digest)) ||
         get_string(&r, next.pending_digest, sizeof(next.pending_digest)) ||
         get_string(&r, next.error, sizeof(next.error)))) return -1;
    if (format == LCS_CONFIG_STATUS_COMMANDS_FORMAT && (lcs_buf_get_u8(&r, &next.dynamic_commands) || next.dynamic_commands != 1)) return -1;
    if (r.off != r.len || !view_valid(&next))
        return -1;
    *view = next;
    return 0;
}

const char *lcs_config_status_phase_name(uint16_t phase)
{
    static const char *names[] = {"legacy", "recovering", "blocked", "admitting", "active", "failed", "stopping"};
    return phase < sizeof(names) / sizeof(*names) ? names[phase] : "unknown";
}

bool lcs_config_status_node_matches(const lcs_config_status_view_t *view, size_t node)
{
    if (node >= view->count || view->self >= view->count)
        return false;
    const lcs_config_status_node_t *local = &view->nodes[view->self], *remote = &view->nodes[node];
    return local->known && local->status.prepared && remote->connected && remote->known && remote->status.prepared &&
        !strcmp(local->status.membership, remote->status.membership) && !strcmp(local->status.baseline, remote->status.baseline);
}
