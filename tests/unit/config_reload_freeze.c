// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_reload.h"
#include "protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

daemon_state_t g_state;
void lcs_log_warn(const char *format, ...) { (void)format; }
void lcs_log_info(const char *format, ...) { (void)format; }

int main(void)
{
    lcs_config_init_defaults(&g_state.cfg);
    strcpy(g_state.cfg.cluster_name, "freeze-test");
    strcpy(g_state.cfg.self_name, "node0");
    g_state.cfg.node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(g_state.cfg.nodes[i].name, sizeof(g_state.cfg.nodes[i].name), "node%zu", i);
        strcpy(g_state.cfg.nodes[i].address, "127.0.0.1");
        g_state.cfg.nodes[i].port = 3322;
        g_state.cfg.nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    char error[256];
    assert(!lcs_config_finalize(&g_state.cfg, error, sizeof(error)));
    g_state.quorum_needed = 2;
    config_reload_init("");
    assert(!config_reload_definitions_frozen());
    config_reload_request();
    assert(g_state.config_reload.request_pending);
    unsigned char payload[64];
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, sizeof(payload));
    assert(!lcs_buf_put_u8(&writer, 1) && !lcs_buf_put_u8(&writer, 0) && !lcs_buf_put_u8(&writer, 0));
    assert(!lcs_buf_put_u64(&writer, 123) && !lcs_buf_put_u64(&writer, 456) && !lcs_buf_put_u32(&writer, 7));
    lcs_config_store_t store = {0};
    for (int mode = 0; mode < 4; mode++)
    {
        config_reload_init("");
        g_state.config_bootstrap.initialization_locked = mode == 0;
        g_state.config_runtime.activation.store = mode == 1 ? &store : NULL;
        g_state.config_runtime.barrier.store = mode == 2 ? &store : NULL;
        g_state.config_runtime.requires_recovery = mode == 3;
        assert(config_reload_definitions_frozen());
        config_reload_request();
        assert(!g_state.config_reload.request_pending);
        assert(!config_reload_handle_announcement(payload, writer.len, 1));
        assert(!g_state.config_reload.request_pending && !g_state.config_reload.peer_loaded[1]);
        assert(config_reload_handle_commit(payload, writer.len, 1) == -1);
        assert(!g_state.config_reload.commit_requested);
    }
    puts("frozen configuration reload admission unit tests passed");
    return 0;
}
