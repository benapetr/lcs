// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "vip.h"

#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <linux/rtnetlink.h>
#include <netpacket/packet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct __attribute__((packed))
{
    unsigned char dst[ETH_ALEN];
    unsigned char src[ETH_ALEN];
    uint16_t ethertype;
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    unsigned char sha[ETH_ALEN];
    unsigned char spa[4];
    unsigned char tha[ETH_ALEN];
    unsigned char tpa[4];
} lcs_arp_packet_t;

typedef struct
{
    int ifindex;
    unsigned char mac[ETH_ALEN];
    unsigned int flags;
} lcs_if_info_t;

static lcs_vip_backend_t g_backend = LCS_VIP_BACKEND_IP;

void lcs_vip_set_backend(lcs_vip_backend_t backend)
{
    if (backend == LCS_VIP_BACKEND_IP || backend == LCS_VIP_BACKEND_NETLINK)
        g_backend = backend;
}

static int run_ip_addr(const char *op, const lcs_vip_config_t *vip)
{
    if (getenv("LCS_VIP_DRY_RUN"))
    {
        lcs_log_info("dry-run VIP %s %s on %s", op, vip->address, vip->interface);
        return 0;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ip addr %s %s dev %s >/dev/null 2>&1", op, vip->address, vip->interface);
    int rc = system(cmd);
    if (rc != 0)
    {
        lcs_log_warn("VIP %s %s on %s failed with rc=%d", op, vip->address, vip->interface, rc);
        return -1;
    }
    lcs_log_info("VIP %s %s on %s", op, vip->address, vip->interface);
    return 0;
}

static int parse_vip_addr_prefix(const char *address, int *family, unsigned char *addr, size_t addr_len, uint8_t *prefix_len)
{
    char buf[LCS_ADDR_MAX + 1];
    snprintf(buf, sizeof(buf), "%s", address);
    char *slash = strrchr(buf, '/');
    if (!slash)
        return -1;
    *slash++ = '\0';
    char *end = NULL;
    unsigned long prefix = strtoul(slash, &end, 10);
    if (!end || *end != '\0')
        return -1;
    struct in_addr a4;
    if (inet_pton(AF_INET, buf, &a4) == 1)
    {
        if (prefix > 32 || addr_len < sizeof(a4))
            return -1;
        *family = AF_INET;
        *prefix_len = (uint8_t)prefix;
        memcpy(addr, &a4, sizeof(a4));
        return 0;
    }
    struct in6_addr a6;
    if (inet_pton(AF_INET6, buf, &a6) == 1)
    {
        if (prefix > 128 || addr_len < sizeof(a6))
            return -1;
        *family = AF_INET6;
        *prefix_len = (uint8_t)prefix;
        memcpy(addr, &a6, sizeof(a6));
        return 0;
    }
    return -1;
}

static int add_rtattr(struct nlmsghdr *nlh, size_t max_len, int type, const void *data, size_t data_len)
{
    size_t len = RTA_LENGTH(data_len);
    size_t new_len = NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(len);
    if (new_len > max_len)
        return -1;
    struct rtattr *rta = (struct rtattr *)((char *)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)len;
    memcpy(RTA_DATA(rta), data, data_len);
    nlh->nlmsg_len = (uint32_t)new_len;
    return 0;
}

static int netlink_addr_op(const char *op, const lcs_vip_config_t *vip, bool add)
{
    if (getenv("LCS_VIP_DRY_RUN"))
    {
        lcs_log_info("dry-run netlink VIP %s %s on %s", op, vip->address, vip->interface);
        return 0;
    }
    unsigned char addr[sizeof(struct in6_addr)];
    int family = AF_UNSPEC;
    uint8_t prefix_len = 0;
    if (parse_vip_addr_prefix(vip->address, &family, addr, sizeof(addr), &prefix_len) != 0)
        return -1;
    unsigned int ifindex = if_nametoindex(vip->interface);
    if (!ifindex)
    {
        lcs_log_warn("netlink VIP %s %s on %s failed: unknown interface", op, vip->address, vip->interface);
        return -1;
    }

    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        lcs_log_warn("netlink VIP %s %s on %s failed: %s", op, vip->address, vip->interface, strerror(errno));
        return -1;
    }

    struct {
        struct nlmsghdr nlh;
        struct ifaddrmsg ifa;
        char attrs[256];
    } req;
    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.ifa));
    req.nlh.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (add)
        req.nlh.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
    req.ifa.ifa_family = (unsigned char)family;
    req.ifa.ifa_prefixlen = prefix_len;
    req.ifa.ifa_scope = family == AF_INET ? RT_SCOPE_UNIVERSE : RT_SCOPE_UNIVERSE;
    req.ifa.ifa_index = ifindex;

    size_t addr_len = family == AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
    if (add_rtattr(&req.nlh, sizeof(req), IFA_LOCAL, addr, addr_len) != 0 ||
        add_rtattr(&req.nlh, sizeof(req), IFA_ADDRESS, addr, addr_len) != 0)
    {
        close(fd);
        return -1;
    }

    struct sockaddr_nl nladdr;
    memset(&nladdr, 0, sizeof(nladdr));
    nladdr.nl_family = AF_NETLINK;
    if (sendto(fd, &req, req.nlh.nlmsg_len, 0, (struct sockaddr *)&nladdr, sizeof(nladdr)) < 0)
    {
        lcs_log_warn("netlink VIP %s %s on %s failed: %s", op, vip->address, vip->interface, strerror(errno));
        close(fd);
        return -1;
    }

    char resp[8192];
    ssize_t n = recv(fd, resp, sizeof(resp), 0);
    if (n < 0)
    {
        lcs_log_warn("netlink VIP %s %s on %s ack failed: %s", op, vip->address, vip->interface, strerror(errno));
        close(fd);
        return -1;
    }
    for (struct nlmsghdr *nlh = (struct nlmsghdr *)resp; NLMSG_OK(nlh, (unsigned int)n); nlh = NLMSG_NEXT(nlh, n))
    {
        if (nlh->nlmsg_type == NLMSG_ERROR)
        {
            struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(nlh);
            if (err->error == 0)
            {
                close(fd);
                lcs_log_info("netlink VIP %s %s on %s", op, vip->address, vip->interface);
                return 0;
            }
            if (!add && err->error == -EADDRNOTAVAIL)
            {
                close(fd);
                lcs_log_info("netlink VIP %s %s on %s already absent", op, vip->address, vip->interface);
                return 0;
            }
            errno = -err->error;
            lcs_log_warn("netlink VIP %s %s on %s failed: %s", op, vip->address, vip->interface, strerror(errno));
            close(fd);
            return -1;
        }
    }
    close(fd);
    return -1;
}

int lcs_vip_add(const lcs_vip_config_t *vip)
{
    if (g_backend == LCS_VIP_BACKEND_NETLINK)
        return netlink_addr_op("add", vip, true);
    return run_ip_addr("add", vip);
}

int lcs_vip_del(const lcs_vip_config_t *vip)
{
    if (g_backend == LCS_VIP_BACKEND_NETLINK)
        return netlink_addr_op("del", vip, false);
    return run_ip_addr("del", vip);
}

static int parse_vip_ipv4(const char *address, struct in_addr *addr)
{
    char buf[LCS_ADDR_MAX + 1];
    snprintf(buf, sizeof(buf), "%s", address);
    char *slash = strchr(buf, '/');
    if (slash)
        *slash = '\0';
    return inet_pton(AF_INET, buf, addr) == 1 ? 0 : -1;
}

static int parse_vip_ipv6(const char *address, struct in6_addr *addr)
{
    char buf[LCS_ADDR_MAX + 1];
    snprintf(buf, sizeof(buf), "%s", address);
    char *slash = strchr(buf, '/');
    if (slash)
        *slash = '\0';
    return inet_pton(AF_INET6, buf, addr) == 1 ? 0 : -1;
}

static int get_if_info(const char *ifname, lcs_if_info_t *info)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) != 0)
    {
        close(fd);
        return -1;
    }
    info->ifindex = ifr.ifr_ifindex;
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0)
    {
        close(fd);
        return -1;
    }
    info->flags = (unsigned int)ifr.ifr_flags;
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) != 0)
    {
        close(fd);
        return -1;
    }
    if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
    {
        close(fd);
        return 1;
    }
    memcpy(info->mac, ifr.ifr_hwaddr.sa_data, ETH_ALEN);
    close(fd);
    return 0;
}

static int open_arp_socket(int ifindex)
{
    int fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC, htons(ETH_P_ARP));
    if (fd < 0)
        return -1;
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(ETH_P_ARP);
    addr.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static void fill_arp_packet(lcs_arp_packet_t *pkt, const unsigned char mac[ETH_ALEN],
                            const struct in_addr *sender_ip,
                            const struct in_addr *target_ip, uint16_t op)
{
    static const unsigned char broadcast[ETH_ALEN] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff
    };
    memset(pkt, 0, sizeof(*pkt));
    memcpy(pkt->dst, broadcast, ETH_ALEN);
    memcpy(pkt->src, mac, ETH_ALEN);
    pkt->ethertype = htons(ETH_P_ARP);
    pkt->htype = htons(ARPHRD_ETHER);
    pkt->ptype = htons(ETH_P_IP);
    pkt->hlen = ETH_ALEN;
    pkt->plen = 4;
    pkt->oper = htons(op);
    memcpy(pkt->sha, mac, ETH_ALEN);
    memcpy(pkt->spa, &sender_ip->s_addr, sizeof(pkt->spa));
    memcpy(pkt->tpa, &target_ip->s_addr, sizeof(pkt->tpa));
}

static int send_arp_packet(int fd, int ifindex, const lcs_arp_packet_t *pkt)
{
    struct sockaddr_ll addr;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_ifindex = ifindex;
    addr.sll_halen = ETH_ALEN;
    memset(addr.sll_addr, 0xff, ETH_ALEN);
    ssize_t n = sendto(fd, pkt, sizeof(*pkt), 0, (struct sockaddr *)&addr, sizeof(addr));
    return n == (ssize_t)sizeof(*pkt) ? 0 : -1;
}

static int arp_packet_claims_ip(const unsigned char *buf, ssize_t len, const struct in_addr *ip, const unsigned char own_mac[ETH_ALEN])
{
    if (len < (ssize_t)sizeof(lcs_arp_packet_t))
        return 0;
        
    const lcs_arp_packet_t *pkt = (const lcs_arp_packet_t *)buf;
    if (ntohs(pkt->ethertype) != ETH_P_ARP ||
        ntohs(pkt->htype) != ARPHRD_ETHER ||
        ntohs(pkt->ptype) != ETH_P_IP ||
        pkt->hlen != ETH_ALEN ||
        pkt->plen != 4)
        {
        return 0;
    }
    if (memcmp(pkt->sha, own_mac, ETH_ALEN) == 0)
        return 0;
    return memcmp(pkt->spa, &ip->s_addr, sizeof(pkt->spa)) == 0;
}

static void ipv6_solicited_node_multicast(const struct in6_addr *target, struct in6_addr *multicast)
{
    memset(multicast, 0, sizeof(*multicast));
    multicast->s6_addr[0] = 0xff;
    multicast->s6_addr[1] = 0x02;
    multicast->s6_addr[11] = 0x01;
    multicast->s6_addr[12] = 0xff;
    multicast->s6_addr[13] = target->s6_addr[13];
    multicast->s6_addr[14] = target->s6_addr[14];
    multicast->s6_addr[15] = target->s6_addr[15];
}

static int open_icmp6_socket(unsigned int ifindex)
{
    int fd = socket(AF_INET6, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_ICMPV6);
    if (fd < 0)
        return -1;

    int hops = 255;
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops));
    setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &hops, sizeof(hops));
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifindex, sizeof(ifindex));
    return fd;
}

static int send_neighbor_solicitation(int fd, unsigned int ifindex, const struct in6_addr *target)
{
    struct {
        struct nd_neighbor_solicit ns;
    } pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.ns.nd_ns_type = ND_NEIGHBOR_SOLICIT;
    pkt.ns.nd_ns_code = 0;
    pkt.ns.nd_ns_target = *target;

    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_scope_id = ifindex;
    ipv6_solicited_node_multicast(target, &dst.sin6_addr);
    ssize_t n = sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    return n == (ssize_t)sizeof(pkt) ? 0 : -1;
}

static int send_neighbor_advertisement(int fd, unsigned int ifindex, const struct in6_addr *target, const unsigned char mac[ETH_ALEN])
{
    struct {
        struct nd_neighbor_advert na;
        uint8_t opt_type;
        uint8_t opt_len;
        unsigned char opt_mac[ETH_ALEN];
    } pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.na.nd_na_type = ND_NEIGHBOR_ADVERT;
    pkt.na.nd_na_code = 0;
    pkt.na.nd_na_flags_reserved = htonl(ND_NA_FLAG_OVERRIDE);
    pkt.na.nd_na_target = *target;
    pkt.opt_type = ND_OPT_TARGET_LINKADDR;
    pkt.opt_len = 1;
    memcpy(pkt.opt_mac, mac, ETH_ALEN);

    struct sockaddr_in6 dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin6_family = AF_INET6;
    dst.sin6_scope_id = ifindex;
    inet_pton(AF_INET6, "ff02::1", &dst.sin6_addr);
    ssize_t n = sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    return n == (ssize_t)sizeof(pkt) ? 0 : -1;
}

static int icmp6_advertises_target(const unsigned char *buf, ssize_t len, const struct in6_addr *target)
{
    if (len < (ssize_t)sizeof(struct nd_neighbor_advert))
        return 0;
    const struct nd_neighbor_advert *na = (const struct nd_neighbor_advert *)buf;
    if (na->nd_na_type != ND_NEIGHBOR_ADVERT || na->nd_na_code != 0)
        return 0;
    return memcmp(&na->nd_na_target, target, sizeof(*target)) == 0;
}

static int nd_conflict_check(const lcs_config_t *cfg, const lcs_vip_config_t *vip, const struct in6_addr *vip_ip)
{
    lcs_if_info_t info;
    memset(&info, 0, sizeof(info));
    if (get_if_info(vip->interface, &info) < 0)
    {
        lcs_log_warn("failed to inspect interface %s for IPv6 ND probing: %s", vip->interface, strerror(errno));
        return -1;
    }
    if (info.flags & IFF_LOOPBACK)
    {
        lcs_log_debug("skipping IPv6 ND conflict check for %s on loopback interface %s", vip->address, vip->interface);
        return 0;
    }
    if (!(info.flags & IFF_UP))
    {
        lcs_log_warn("interface %s is down; cannot ND-probe VIP %s", vip->interface, vip->address);
        return -1;
    }
    int fd = open_icmp6_socket((unsigned int)info.ifindex);
    if (fd < 0)
    {
        lcs_log_warn("failed to open ICMPv6 socket on %s: %s", vip->interface, strerror(errno));
        return -1;
    }
    uint32_t probe_count = cfg->probe_count ? cfg->probe_count : 1;
    uint32_t timeout_ms = cfg->probe_timeout_ms ? cfg->probe_timeout_ms : 300;
    unsigned char buf[2048];
    for (uint32_t i = 0; i < probe_count; i++)
    {
        if (send_neighbor_solicitation(fd, (unsigned int)info.ifindex, vip_ip) != 0)
        {
            lcs_log_warn("failed to send ND probe for %s on %s: %s", vip->address, vip->interface, strerror(errno));
            close(fd);
            return -1;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, (int)timeout_ms);
        while (rc > 0)
        {
            ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                close(fd);
                return -1;
            }
            if (icmp6_advertises_target(buf, n, vip_ip))
            {
                lcs_log_warn("IPv6 ND conflict detected for VIP %s on %s", vip->address, vip->interface);
                close(fd);
                return 1;
            }
            rc = poll(&pfd, 1, 0);
        }
    }
    close(fd);
    return 0;
}

static int nd_announce(const lcs_config_t *cfg, const lcs_vip_config_t *vip, const struct in6_addr *vip_ip)
{
    lcs_if_info_t info;
    memset(&info, 0, sizeof(info));
    int if_rc = get_if_info(vip->interface, &info);
    if (if_rc != 0 || (info.flags & IFF_LOOPBACK))
        return 0;

    if (!(info.flags & IFF_UP))
        return -1;

    int fd = open_icmp6_socket((unsigned int)info.ifindex);
    if (fd < 0)
        return -1;

    uint32_t count = cfg->probe_count ? cfg->probe_count : 1;
    for (uint32_t i = 0; i < count; i++)
    {
        if (send_neighbor_advertisement(fd, (unsigned int)info.ifindex, vip_ip, info.mac) != 0)
        {
            close(fd);
            return -1;
        }
        usleep(50000);
    }
    close(fd);
    lcs_log_info("sent unsolicited Neighbor Advertisement for VIP %s on %s", vip->address, vip->interface);
    return 0;
}

int lcs_vip_conflict_check(const lcs_config_t *cfg, const lcs_vip_config_t *vip)
{
    if (getenv("LCS_VIP_CONFLICT"))
    {
        lcs_log_warn("dry-run VIP conflict detected for %s on %s", vip->address, vip->interface);
        return 1;
    }
    if (getenv("LCS_VIP_DRY_RUN"))
    {
        lcs_log_info("dry-run VIP conflict check %s on %s", vip->address, vip->interface);
        return 0;
    }

    struct in_addr vip_ip;
    if (parse_vip_ipv4(vip->address, &vip_ip) != 0)
    {
        struct in6_addr vip_ip6;
        if (parse_vip_ipv6(vip->address, &vip_ip6) == 0)
            return nd_conflict_check(cfg, vip, &vip_ip6);

        return -1;
    }

    lcs_if_info_t info;
    memset(&info, 0, sizeof(info));
    int if_rc = get_if_info(vip->interface, &info);
    if (if_rc != 0)
    {
        if (if_rc > 0)
        {
            lcs_log_debug("skipping ARP conflict check for %s on non-Ethernet interface %s", vip->address, vip->interface);
            return 0;
        }
        lcs_log_warn("failed to inspect interface %s for ARP probing: %s", vip->interface, strerror(errno));
        return -1;
    }
    if (info.flags & IFF_LOOPBACK)
    {
        lcs_log_debug("skipping ARP conflict check for %s on loopback interface %s", vip->address, vip->interface);
        return 0;
    }
    if (!(info.flags & IFF_UP))
    {
        lcs_log_warn("interface %s is down; cannot ARP-probe VIP %s", vip->interface, vip->address);
        return -1;
    }

    int fd = open_arp_socket(info.ifindex);
    if (fd < 0)
    {
        lcs_log_warn("failed to open ARP socket on %s: %s", vip->interface, strerror(errno));
        return -1;
    }

    struct in_addr zero;
    zero.s_addr = 0;
    lcs_arp_packet_t probe;
    fill_arp_packet(&probe, info.mac, &zero, &vip_ip, ARPOP_REQUEST);
    unsigned char buf[2048];
    uint32_t probe_count = cfg->probe_count ? cfg->probe_count : 1;
    uint32_t timeout_ms = cfg->probe_timeout_ms ? cfg->probe_timeout_ms : 300;
    for (uint32_t i = 0; i < probe_count; i++)
    {
        if (send_arp_packet(fd, info.ifindex, &probe) != 0)
        {
            lcs_log_warn("failed to send ARP probe for %s on %s: %s", vip->address, vip->interface, strerror(errno));
            close(fd);
            return -1;
        }
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int rc = poll(&pfd, 1, (int)timeout_ms);
        while (rc > 0)
        {
            ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                close(fd);
                return -1;
            }
            if (arp_packet_claims_ip(buf, n, &vip_ip, info.mac))
            {
                lcs_log_warn("ARP conflict detected for VIP %s on %s", vip->address, vip->interface);
                close(fd);
                return 1;
            }
            rc = poll(&pfd, 1, 0);
        }
    }
    close(fd);
    return 0;
}

int lcs_vip_announce(const lcs_config_t *cfg, const lcs_vip_config_t *vip)
{
    if (getenv("LCS_VIP_DRY_RUN"))
    {
        lcs_log_info("dry-run gratuitous ARP %s on %s", vip->address, vip->interface);
        return 0;
    }
    struct in_addr vip_ip;
    if (parse_vip_ipv4(vip->address, &vip_ip) != 0)
    {
        struct in6_addr vip_ip6;
        if (parse_vip_ipv6(vip->address, &vip_ip6) == 0)
            return nd_announce(cfg, vip, &vip_ip6);
        return -1;
    }
    lcs_if_info_t info;
    memset(&info, 0, sizeof(info));
    int if_rc = get_if_info(vip->interface, &info);
    if (if_rc != 0 || (info.flags & IFF_LOOPBACK))
        return 0;
    int fd = open_arp_socket(info.ifindex);
    if (fd < 0)
        return -1;
    lcs_arp_packet_t pkt;
    fill_arp_packet(&pkt, info.mac, &vip_ip, &vip_ip, ARPOP_REQUEST);
    uint32_t count = cfg->probe_count ? cfg->probe_count : 1;
    for (uint32_t i = 0; i < count; i++)
    {
        if (send_arp_packet(fd, info.ifindex, &pkt) != 0)
        {
            close(fd);
            return -1;
        }
        usleep(50000);
    }
    close(fd);
    lcs_log_info("sent gratuitous ARP for VIP %s on %s", vip->address, vip->interface);
    return 0;
}
