// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cluster.h"
#include "config.h"
#include "daemon_state.h"
#include "epoll_util.h"
#include "group.h"
#include "lease.h"
#include "local_client.h"
#include "log.h"
#include "metrics.h"
#include "peer.h"
#include "resources.h"
#include "scheduler.h"
#include "util.h"
#include "vip.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

volatile sig_atomic_t g_stop;
daemon_state_t g_state;
int g_peer_listener_fd = -1;
int g_local_fd = -1;
int g_tcp_fd = -1;
int g_metrics_fd = -1;

typedef struct
{
    const char *config_path;
    bool foreground;
    bool daemonize;
    bool no_syslog;
    bool no_timestamp;
    int verbosity;
    bool exit_now;
    int exit_code;
} daemon_options_t;

static void close_listener_fds(void)
{
    if (g_local_fd >= 0)
    {
        close(g_local_fd);
        g_local_fd = -1;
    }
    if (g_tcp_fd >= 0)
    {
        close(g_tcp_fd);
        g_tcp_fd = -1;
    }
    if (g_metrics_fd >= 0)
    {
        close(g_metrics_fd);
        g_metrics_fd = -1;
    }
    g_peer_listener_fd = -1;
}

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: lcsd [--version]\n");
    fprintf(out, "       lcsd -c CONFIG [-f|--foreground] [--daemonize] [--no-syslog] [--no-timestamp] [-v|--verbose] [-vv] [-vvv]\n");
}

static int daemonize_process(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid > 0)
        _exit(0);

    if (setsid() < 0)
        return -1;

    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0)
        return -1;

    if (pid > 0)
        _exit(0);

    umask(027);
    if (chdir("/") != 0)
        return -1;

    int fd = open("/dev/null", O_RDWR);
    if (fd < 0)
        return -1;

    dup2(fd, STDIN_FILENO);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);

    return 0;
}

static int write_pidfile(const char *path)
{
    if (!path || !*path)
        return 0;

    if (lcs_mkdir_parent(path, 0755) != 0)
        return -1;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;

    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
    int rc = (len > 0 && write(fd, buf, (size_t)len) == len) ? 0 : -1;
    close(fd);
    return rc;
}

static int create_unix_listener(const char *path)
{
    if (lcs_mkdir_parent(path, 0755) != 0)
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path))
    {
        errno = ENAMETOOLONG;
        close(fd);
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int create_tcp_listener(const char *bind_address, uint16_t port)
{
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%u", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    struct addrinfo *res = NULL;
    int gai = getaddrinfo(*bind_address ? bind_address : NULL, port_buf, &hints, &res);
    if (gai != 0)
    {
        errno = EINVAL;
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
            continue;

        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 && listen(fd, 16) == 0)
            break;

        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int lcs_init_metrics_socket(void)
{
    if (g_state.cfg.metrics_enabled)
    {
        g_metrics_fd = create_tcp_listener(g_state.cfg.metrics_bind_address, g_state.cfg.metrics_port);
        if (g_metrics_fd < 0)
        {
            lcs_log_error("failed to listen on metrics HTTP %s:%u: %s", g_state.cfg.metrics_bind_address, g_state.cfg.metrics_port, strerror(errno));
            close_listener_fds();
            unlink(g_state.cfg.socket_path);
            return 1;
        }
        if (lcs_set_fd_nonblocking(g_metrics_fd) != 0)
        {
            lcs_log_error("failed to set metrics listener nonblocking: %s", strerror(errno));
            close_listener_fds();
            unlink(g_state.cfg.socket_path);
            return 1;
        }
    }
    return 0;
}

static int lcs_init_open_sockets(void)
{
    g_local_fd = create_unix_listener(g_state.cfg.socket_path);
    if (g_local_fd < 0)
    {
        lcs_log_error("failed to listen on %s: %s", g_state.cfg.socket_path, strerror(errno));
        return 1;
    }
    if (lcs_set_fd_nonblocking(g_local_fd) != 0)
    {
        lcs_log_error("failed to set local listener nonblocking: %s", strerror(errno));
        close_listener_fds();
        unlink(g_state.cfg.socket_path);
        return 1;
    }
    g_tcp_fd = create_tcp_listener(g_state.cfg.bind_address, g_state.cfg.port);
    if (g_tcp_fd < 0)
    {
        lcs_log_error("failed to listen on TCP %s:%u: %s", *g_state.cfg.bind_address ? g_state.cfg.bind_address : "*", g_state.cfg.port, strerror(errno));
        close_listener_fds();
        unlink(g_state.cfg.socket_path);
        return 1;
    }
    if (lcs_set_fd_nonblocking(g_tcp_fd) != 0)
    {
        lcs_log_error("failed to set TCP listener nonblocking: %s", strerror(errno));
        close_listener_fds();
        unlink(g_state.cfg.socket_path);
        return 1;
    }
    return 0;
}

static void daemon_options_init(daemon_options_t *opts)
{
    memset(opts, 0, sizeof(*opts));
    opts->config_path = LCS_DEFAULT_CONFIG_PATH;
}

static int parse_daemon_args(int argc, char **argv, daemon_options_t *opts)
{
    static const struct option long_opts[] = {
        { "config",       required_argument, NULL, 'c' },
        { "daemonize",    no_argument,       NULL, 'd' },
        { "foreground",   no_argument,       NULL, 'f' },
        { "stdout",       no_argument,       NULL, 'f' },
        { "no-syslog",    no_argument,       NULL, 'S' },
        { "no-timestamp", no_argument,       NULL, 'T' },
        { "verbose",      no_argument,       NULL, 'v' },
        { "version",      no_argument,       NULL, 'V' },
        { "help",         no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "c:dfvhV", long_opts, NULL)) != -1)
    {
        switch (opt)
        {
            case 'c':
                opts->config_path = optarg;
                break;
            case 'd':
                opts->daemonize = true;
                break;
            case 'f':
                opts->foreground = true;
                break;
            case 'S':
                opts->no_syslog = true;
                break;
            case 'T':
                opts->no_timestamp = true;
                break;
            case 'v':
                opts->verbosity++;
                break;
            case 'V':
                printf("lcsd %s\n", LCS_VERSION);
                opts->exit_now = true;
                opts->exit_code = 0;
                return 0;
            case 'h':
                usage(stdout);
                opts->exit_now = true;
                opts->exit_code = 0;
                return 0;
            default:
                usage(stderr);
                return 2;
        }
    }

    if (opts->daemonize && opts->foreground)
    {
        fprintf(stderr, "lcsd: --daemonize cannot be combined with --foreground/--stdout\n");
        return 2;
    }
    return 0;
}

static int load_daemon_config(const daemon_options_t *opts)
{
    memset(&g_state, 0, sizeof(g_state));
    char err[256] = {0};
    if (lcs_config_load(opts->config_path, &g_state.cfg, err, sizeof(err)) != 0)
    {
        lcs_log_open("lcsd", opts->foreground, opts->verbosity, !opts->no_syslog, !opts->no_timestamp);
        lcs_log_error("config error: %s", err);
        lcs_log_close();
        return -1;
    }
    return 0;
}

static int enter_runtime_mode(const daemon_options_t *opts)
{
    if (opts->daemonize && daemonize_process() != 0)
    {
        fprintf(stderr, "lcsd: failed to daemonize: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static bool open_daemon_log(const daemon_options_t *opts)
{
    bool syslog_enabled = g_state.cfg.syslog_enabled && !opts->no_syslog;
    lcs_log_open("lcsd", opts->foreground, opts->verbosity, syslog_enabled,
                 !opts->no_timestamp);
    return syslog_enabled;
}

static void initialize_daemon_state(void)
{
    lcs_vip_set_backend(g_state.cfg.vip_backend);
    g_state.self_index = lcs_config_self_index(&g_state.cfg);
    g_state.instance_id = lcs_random_u64();
    g_state.quorum_needed = lcs_config_quorum(&g_state.cfg);
    g_state.votes_seen = 1;

    for (size_t i = 0; i < g_state.cfg.node_count; i++)
        g_state.peers[i].fd = -1;

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        g_state.resources[i].owner_node = -1;
        g_state.resources[i].owner_instance_id = 0;
        g_state.resources[i].state = LCS_RES_STOPPED;
    }
}

static void log_startup_config(const daemon_options_t *opts, bool syslog_enabled)
{
    lcs_log_info("startup config: config=%s foreground=%s daemonize=%s syslog=%s stdout_timestamp=%s vip_backend=%s verbosity=%d node=%s self_index=%d cluster=%s",
                 opts->config_path,
                 opts->foreground ? "true" : "false",
                 opts->daemonize ? "true" : "false",
                 syslog_enabled ? "true" : "false",
                 opts->no_timestamp ? "false" : "true",
                 g_state.cfg.vip_backend == LCS_VIP_BACKEND_NETLINK ? "netlink" : "ip",
                 opts->verbosity,
                 g_state.cfg.self_name,
                 g_state.self_index,
                 *g_state.cfg.cluster_name ? g_state.cfg.cluster_name : "-");
    lcs_log_info("startup config: bind=%s port=%u socket=%s pidfile=%s metrics=%s metrics_bind=%s metrics_port=%u nodes=%zu vips=%zu quorum_needed=%u",
                 *g_state.cfg.bind_address ? g_state.cfg.bind_address : "*",
                 g_state.cfg.port,
                 g_state.cfg.socket_path,
                 *g_state.cfg.pidfile_path ? g_state.cfg.pidfile_path : "-",
                 g_state.cfg.metrics_enabled ? "true" : "false",
                 g_state.cfg.metrics_enabled ? g_state.cfg.metrics_bind_address : "-",
                 g_state.cfg.metrics_port,
                 g_state.cfg.node_count,
                 g_state.cfg.vip_count,
                 g_state.quorum_needed);
    lcs_log_info("startup config: lease_ms=%u renew_ms=%u peer_timeout_ms=%u probe_count=%u probe_timeout_ms=%u hook_timeout_ms=%u secret_configured=%s",
                 g_state.cfg.lease_ms,
                 g_state.cfg.renew_ms,
                 g_state.cfg.peer_timeout_ms,
                 g_state.cfg.probe_count,
                 g_state.cfg.probe_timeout_ms,
                 g_state.cfg.hook_timeout_ms,
                 *g_state.cfg.secret ? "true" : "false");
}

static int setup_runtime(int *epoll_fd)
{
    if (install_signal_handlers() != 0)
    {
        lcs_log_error("failed to install signal handlers: %s", strerror(errno));
        return -1;
    }

    resources_cleanup_local_vips_without_lease();

    if (lcs_init_open_sockets() != 0)
        return -1;

    g_peer_listener_fd = g_tcp_fd;

    if (lcs_init_metrics_socket() != 0)
        return -1;

    *epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (*epoll_fd < 0)
    {
        lcs_log_error("failed to create epoll instance: %s", strerror(errno));
        close_listener_fds();
        unlink(g_state.cfg.socket_path);
        return -1;
    }
    if (lcs_add_epoll_fd(*epoll_fd, g_local_fd, LCS_EPOLL_LOCAL) != 0 ||
        lcs_add_epoll_fd(*epoll_fd, g_tcp_fd, LCS_EPOLL_PEER) != 0 ||
        (g_metrics_fd >= 0 && lcs_add_epoll_fd(*epoll_fd, g_metrics_fd, LCS_EPOLL_METRICS) != 0))
    {
        lcs_log_error("failed to register listener with epoll: %s", strerror(errno));
        close(*epoll_fd);
        *epoll_fd = -1;
        close_listener_fds();
        unlink(g_state.cfg.socket_path);
        return -1;
    }
    if (write_pidfile(g_state.cfg.pidfile_path) != 0)
    {
        lcs_log_error("failed to write pidfile %s: %s", g_state.cfg.pidfile_path, strerror(errno));
        close(*epoll_fd);
        *epoll_fd = -1;
        close_listener_fds();
        unlink(g_state.cfg.socket_path);
        return -1;
    }
    return 0;
}

static void log_daemon_started(void)
{
    lcs_log_info("lcsd started node=%s instance=%llu socket=%s tcp=%s:%u metrics=%s:%u quorum=%u/%zu",
                 g_state.cfg.self_name, (unsigned long long)g_state.instance_id,
                 g_state.cfg.socket_path,
                 *g_state.cfg.bind_address ? g_state.cfg.bind_address : "*", g_state.cfg.port,
                 g_metrics_fd >= 0 ? g_state.cfg.metrics_bind_address : "-",
                 g_metrics_fd >= 0 ? g_state.cfg.metrics_port : 0,
                 g_state.quorum_needed, g_state.cfg.node_count);
}

static void run_daemon_loop(int epoll_fd)
{
    scheduler_t sched = {
        .epoll_fd = epoll_fd,
        .local_fd = g_local_fd,
        .metrics_fd = g_metrics_fd,
    };

    // Main loop
    while (!g_stop)
    {
        if (scheduler_run_once(&sched) != 0)
            break;
    }
}

static void shutdown_daemon(int epoll_fd)
{
    resources_graceful_shutdown(epoll_fd);
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i != g_state.self_index && g_state.peers[i].fd >= 0)
            peer_close_connection(epoll_fd, (int)i, false, NULL);
    }
    for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++)
    {
        if (g_state.handshakes[i].active)
            handshake_close(epoll_fd, (int)i, "shutdown");
    }
    client_close_all(epoll_fd);
    close(epoll_fd);
    close_listener_fds();
    unlink(g_state.cfg.socket_path);
    if (*g_state.cfg.pidfile_path)
        unlink(g_state.cfg.pidfile_path);
    lcs_log_info("lcsd stopped");
    lcs_log_close();
}

int main(int argc, char **argv)
{
    daemon_options_t opts;
    daemon_options_init(&opts);
    int arg_rc = parse_daemon_args(argc, argv, &opts);
    if (arg_rc != 0)
        return arg_rc;
    if (opts.exit_now)
        return opts.exit_code;

    if (load_daemon_config(&opts) != 0)
        return 1;

    if (enter_runtime_mode(&opts) != 0)
        return 1;

    bool syslog_enabled = open_daemon_log(&opts);
    initialize_daemon_state();
    log_startup_config(&opts, syslog_enabled);
    group_log_strict_warnings();

    int epoll_fd = -1;
    if (setup_runtime(&epoll_fd) != 0)
    {
        lcs_log_close();
        return 1;
    }

    log_daemon_started();
    run_daemon_loop(epoll_fd);
    shutdown_daemon(epoll_fd);
    return 0;
}
