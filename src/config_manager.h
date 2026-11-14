// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_MANAGER_H
#define LCS_CONFIG_MANAGER_H
#include <stdbool.h>
#include <stddef.h>
#include "config_status_protocol.h"
#include "config_agreement.h"

/* Daemon-owned cold entry for activated catalogs or a learned initial activation.
 * Runs before sockets or ordinary workers, freezes legacy bootstrap, and
 * restores durable tables.
 * Does not initiate an activation proposal or first freeze. */
int config_manager_prepare(char *err, size_t err_len);
void config_manager_attach(int epoll_fd);
bool config_manager_enabled(void);
bool config_manager_recovering(void);
/* Live transitions keep backend progress running while placement/CLI edits pause. */
bool config_manager_applying(void);
bool config_manager_failed(void);
/* Fill local diagnostics only; no disk I/O, state advance or peer evidence. */
void config_manager_diagnostics(lcs_config_status_view_t *view);
/* Borrowed exact blocked attempt target, possibly older than committed. No proof
 * of completion or permission to retry. NULL when the error has no bound target. */
const lcs_config_value_t *config_manager_error_target(void);
/* Includes retained historical definitions in the pre-listener capability check. */
int config_manager_has_historical_vips(char *err, size_t err_len);
/* Also attaches a learned activation on an already frozen running node, retaining
 * its coordinator, sessions and resource fences until normal recovery admission. */
void config_manager_process(int epoll_fd);
void config_manager_stop(void);
void config_manager_shutdown_process(void);
bool config_manager_stopped(void);
int config_manager_close(void);
#endif
