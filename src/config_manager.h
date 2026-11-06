// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_MANAGER_H
#define LCS_CONFIG_MANAGER_H
#include <stdbool.h>
#include <stddef.h>
#include "config_status_protocol.h"

/* Daemon-owned entry for existing activated catalogs only. Runs before sockets
 * or ordinary workers, freezes legacy bootstrap, and restores durable tables.
 * Does not activate revision-zero/INI configurations or expose definition edits. */
int config_manager_prepare(char *err, size_t err_len);
void config_manager_attach(int epoll_fd);
bool config_manager_enabled(void);
bool config_manager_recovering(void);
/* Live transitions keep backend progress running while placement/CLI edits pause. */
bool config_manager_applying(void);
bool config_manager_failed(void);
/* Fill local diagnostics only; no disk I/O, state advance or peer evidence. */
void config_manager_diagnostics(lcs_config_status_view_t *view);
/* Includes retained historical definitions in the pre-listener capability check. */
int config_manager_has_historical_vips(char *err, size_t err_len);
void config_manager_process(int epoll_fd);
void config_manager_stop(void);
void config_manager_shutdown_process(void);
bool config_manager_stopped(void);
int config_manager_close(void);
#endif
