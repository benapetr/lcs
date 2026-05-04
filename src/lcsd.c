// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cluster.h"
#include "config.h"
#include "daemon_state.h"
#include "epoll_util.h"
#include "lease.h"
#include "local_client.h"
#include "log.h"
#include "metrics.h"
#include "peer.h"
#include "resources.h"
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
int g_peer_listener_fd = -1;

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

int main(int argc, char **argv)
{
    const char *config_path = LCS_DEFAULT_CONFIG_PATH;
    bool foreground = false;
    bool daemonize = false;
    bool no_syslog = false;
    bool no_timestamp = false;
    int verbosity = 0;
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
                config_path = optarg;
                break;
            case 'd':
                daemonize = true;
                break;
            case 'f':
                foreground = true;
                break;
            case 'S':
                no_syslog = true;
                break;
            case 'T':
                no_timestamp = true;
                break;
            case 'v':
                verbosity++;
                break;
            case 'V':
                printf("lcsd %s\n", LCS_VERSION);
                return 0;
            case 'h':
                usage(stdout);
                return 0;
            default:
                usage(stderr);
                return 2;
        }
    }
    if (daemonize && foreground)
    {
        fprintf(stderr, "lcsd: --daemonize cannot be combined with --foreground/--stdout\n");
        return 2;
    }

    static daemon_state_t st;
    memset(&st, 0, sizeof(st));
    char err[256] = {0};
    if (lcs_config_load(config_path, &st.cfg, err, sizeof(err)) != 0)
    {
        lcs_log_open("lcsd", foreground, verbosity, !no_syslog, !no_timestamp);
        lcs_log_error("config error: %s", err);
        lcs_log_close();
        return 1;
    }
    if (daemonize && daemonize_process() != 0)
    {
        fprintf(stderr, "lcsd: failed to daemonize: %s\n", strerror(errno));
        return 1;
    }
    bool syslog_enabled = st.cfg.syslog_enabled && !no_syslog;
    lcs_log_open("lcsd", foreground, verbosity, syslog_enabled, !no_timestamp);
    lcs_vip_set_backend(st.cfg.vip_backend);
    st.self_index = lcs_config_self_index(&st.cfg);
    st.instance_id = lcs_random_u64();
    st.quorum_needed = lcs_config_quorum(&st.cfg);
    st.votes_seen = 1;
    for (size_t i = 0; i < st.cfg.node_count; i++)
        st.peers[i].fd = -1;
    for (size_t i = 0; i < st.cfg.vip_count; i++)
    {
        st.resources[i].owner_node = -1;
        st.resources[i].owner_instance_id = 0;
        st.resources[i].state = LCS_RES_STOPPED;
    }

    lcs_log_info("startup config: config=%s foreground=%s daemonize=%s syslog=%s stdout_timestamp=%s vip_backend=%s verbosity=%d node=%s self_index=%d cluster=%s",
                 config_path,
                 foreground ? "true" : "false",
                 daemonize ? "true" : "false",
                 syslog_enabled ? "true" : "false",
                 no_timestamp ? "false" : "true",
                 st.cfg.vip_backend == LCS_VIP_BACKEND_NETLINK ? "netlink" : "ip",
                 verbosity,
                 st.cfg.self_name,
                 st.self_index,
                 *st.cfg.cluster_name ? st.cfg.cluster_name : "-");
    lcs_log_info("startup config: bind=%s port=%u socket=%s pidfile=%s metrics=%s metrics_bind=%s metrics_port=%u nodes=%zu vips=%zu quorum_needed=%u",
                 *st.cfg.bind_address ? st.cfg.bind_address : "*",
                 st.cfg.port,
                 st.cfg.socket_path,
                 *st.cfg.pidfile_path ? st.cfg.pidfile_path : "-",
                 st.cfg.metrics_enabled ? "true" : "false",
                 st.cfg.metrics_enabled ? st.cfg.metrics_bind_address : "-",
                 st.cfg.metrics_port,
                 st.cfg.node_count,
                 st.cfg.vip_count,
                 st.quorum_needed);
    lcs_log_info("startup config: lease_ms=%u renew_ms=%u peer_timeout_ms=%u probe_count=%u probe_timeout_ms=%u hook_timeout_ms=%u secret_configured=%s",
                 st.cfg.lease_ms,
                 st.cfg.renew_ms,
                 st.cfg.peer_timeout_ms,
                 st.cfg.probe_count,
                 st.cfg.probe_timeout_ms,
                 st.cfg.hook_timeout_ms,
                 *st.cfg.secret ? "true" : "false");

    if (install_signal_handlers() != 0)
    {
        lcs_log_error("failed to install signal handlers: %s", strerror(errno));
        lcs_log_close();
        return 1;
    }

    cleanup_local_vips_without_lease(&st);

    int local_fd = create_unix_listener(st.cfg.socket_path);
    if (local_fd < 0)
    {
        lcs_log_error("failed to listen on %s: %s", st.cfg.socket_path, strerror(errno));
        return 1;
    }
    int tcp_fd = create_tcp_listener(st.cfg.bind_address, st.cfg.port);
    if (tcp_fd < 0)
    {
        lcs_log_error("failed to listen on TCP %s:%u: %s",
                      *st.cfg.bind_address ? st.cfg.bind_address : "*",
                      st.cfg.port, strerror(errno));
        close(local_fd);
        unlink(st.cfg.socket_path);
        return 1;
    }
    if (set_fd_nonblocking(tcp_fd) != 0)
    {
        lcs_log_error("failed to set TCP listener nonblocking: %s", strerror(errno));
        close(local_fd);
        close(tcp_fd);
        unlink(st.cfg.socket_path);
        return 1;
    }
    g_peer_listener_fd = tcp_fd;
    int metrics_fd = -1;
    if (st.cfg.metrics_enabled)
    {
        metrics_fd = create_tcp_listener(st.cfg.metrics_bind_address, st.cfg.metrics_port);
        if (metrics_fd < 0)
        {
            lcs_log_error("failed to listen on metrics HTTP %s:%u: %s",
                          st.cfg.metrics_bind_address, st.cfg.metrics_port, strerror(errno));
            close(local_fd);
            close(tcp_fd);
            unlink(st.cfg.socket_path);
            return 1;
        }
        if (set_fd_nonblocking(metrics_fd) != 0)
        {
            lcs_log_error("failed to set metrics listener nonblocking: %s", strerror(errno));
            close(local_fd);
            close(tcp_fd);
            close(metrics_fd);
            unlink(st.cfg.socket_path);
            return 1;
        }
    }
    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        lcs_log_error("failed to create epoll instance: %s", strerror(errno));
        close(local_fd);
        close(tcp_fd);
        if (metrics_fd >= 0)
            close(metrics_fd);
        unlink(st.cfg.socket_path);
        return 1;
    }
    if (add_epoll_fd(epoll_fd, local_fd, LCS_EPOLL_LOCAL) != 0 ||
        add_epoll_fd(epoll_fd, tcp_fd, LCS_EPOLL_PEER) != 0 ||
        (metrics_fd >= 0 && add_epoll_fd(epoll_fd, metrics_fd, LCS_EPOLL_METRICS) != 0))
    {
        lcs_log_error("failed to register listener with epoll: %s", strerror(errno));
        close(epoll_fd);
        close(local_fd);
        close(tcp_fd);
        if (metrics_fd >= 0)
            close(metrics_fd);
        unlink(st.cfg.socket_path);
        return 1;
    }
    if (write_pidfile(st.cfg.pidfile_path) != 0)
    {
        lcs_log_error("failed to write pidfile %s: %s", st.cfg.pidfile_path, strerror(errno));
        close(epoll_fd);
        close(local_fd);
        close(tcp_fd);
        if (metrics_fd >= 0)
            close(metrics_fd);
        unlink(st.cfg.socket_path);
        return 1;
    }
    lcs_log_info("lcsd started node=%s instance=%llu socket=%s tcp=%s:%u metrics=%s:%u quorum=%u/%zu",
                 st.cfg.self_name, (unsigned long long)st.instance_id,
                 st.cfg.socket_path,
                 *st.cfg.bind_address ? st.cfg.bind_address : "*", st.cfg.port,
                 metrics_fd >= 0 ? st.cfg.metrics_bind_address : "-",
                 metrics_fd >= 0 ? st.cfg.metrics_port : 0,
                 st.quorum_needed, st.cfg.node_count);

    while (!g_stop)
    {
        struct epoll_event events[64];
        int rc = epoll_wait(epoll_fd, events, 64, LCS_DEFAULT_LOOP_TIMEOUT_MS);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            lcs_log_error("epoll_wait failed: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < rc; i++)
        {
            uint32_t event_id = events[i].data.u32;
            if (event_id == LCS_EPOLL_PEER ||
                (event_id >= LCS_EPOLL_PEER_CONN_BASE &&
                 event_id < LCS_EPOLL_HANDSHAKE_BASE + LCS_HANDSHAKE_MAX))
            {
                pump_peer_epoll_event(&st, epoll_fd, &events[i]);
                continue;
            }
            if (!(events[i].events & EPOLLIN))
                continue;
            if (event_id == LCS_EPOLL_LOCAL)
            {
                int client = accept4(local_fd, NULL, NULL, SOCK_CLOEXEC);
                if (client >= 0)
                {
                    handle_client(client, &st, epoll_fd);
                    close(client);
                }
            } else if (event_id == LCS_EPOLL_METRICS && metrics_fd >= 0)
            {
                for (;;)
                {
                    int client = accept4(metrics_fd, NULL, NULL,
                                         SOCK_CLOEXEC | SOCK_NONBLOCK);
                    if (client < 0)
                    {
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                            lcs_log_debug("metrics accept failed: %s", strerror(errno));
                        break;
                    }
                    handle_metrics_client(client, &st);
                    close(client);
                }
            }
        }
        poll_peers(&st, epoll_fd);
        expire_handshakes(&st, epoll_fd);
        expire_remote_leases(&st);
        process_resource_hooks(&st, epoll_fd);
        maintain_owned_leases(&st, epoll_fd);
        auto_place(&st, epoll_fd);
    }
    graceful_shutdown_resources(&st, epoll_fd);
    for (size_t i = 0; i < st.cfg.node_count; i++)
    {
        if ((int)i != st.self_index && st.peers[i].fd >= 0)
            close_peer_connection(&st, epoll_fd, (int)i, false, NULL);
    }
    for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++)
    {
        if (st.handshakes[i].active)
            close_handshake(&st, epoll_fd, (int)i, "shutdown");
    }
    close(epoll_fd);
    close(local_fd);
    close(tcp_fd);
    if (metrics_fd >= 0)
        close(metrics_fd);
    g_peer_listener_fd = -1;
    unlink(st.cfg.socket_path);
    if (*st.cfg.pidfile_path)
        unlink(st.cfg.pidfile_path);
    lcs_log_info("lcsd stopped");
    lcs_log_close();
    return 0;
}
