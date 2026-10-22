# LCS [Lightweight cluster service]

This is an extremely lightweight GNU/Linux cluster service (similar to pcsd or keepalived) that supports

- Quorum (each node is either full-member or quorum-only)
- Ability to maintain resources such as VIPs and systemd services (only active at one node at a time)
- Optional resource groups for keep-together or anti-affinity placement
- Option to run scripts on failover
- Simple CLI that allows checking current state of cluster
- Prometheus exporter bundled in
- CLI allows moving resources between nodes

It constists of two tiny binaries
- lcs - the CLI tool
- lcsd - daemon service

The goals:
- Solve problems of both pcsd and keepalived
  - Unlike pcsd which pulls about 2GB of deps (on Debian 13) and needs 1G+ of RAM, this needs
    only about 200KB of disk space and ~3MB of RAM
  - Unlike keepalived, this supports real quorum and has many safety checks so it's near impossible
    to have split-brain scenario or IP conflict
  - It's extremely easy to setup and operate reliably
- Written in C - minimal footprint when it comes to RAM and CPU
- Minimal 3rd dependencies - just standard C and GNU/Linux libs

# Building

Just run `make`.

Systemd service resources use D-Bus through sd-bus and are enabled by default on systems with `libsystemd` development headers installed. Build with `make WITH_SYSTEMD=0` to explicitly disable service start/stop/health operations.

# Installing

Copy resulting lcs and lcsd to /usr/local/bin or install precompiled package

lcsd looks for config by default in /etc/lcs/lcs.conf, simple config looks like this (see examples/ for more complex setups)

```
# Comments starting with # or ; are supported on whole lines and after values

[cluster]
name = demo
# Each node has unique name, for example node1, node2, or just a, b, c
node = a
# IP to bind on
bind = 192.168.10.1

# Enable this for prometheus metrics
metrics = true
metrics_bind = 127.0.0.1
metrics_port = 9120

[node a]
role = full-member
address = 192.168.10.1

[node b]
role = full-member
address = 192.168.10.2

[node c]
role = quorum-only
address = 192.168.10.3

[group service]
type = keep-together
mode = best-effort

[vip vip1]
group = service
priority = 1
home_node = a
address = 127.0.0.200/32
interface = lo
# pre_start = /usr/local/libexec/lcs/vip-pre-start
# post_start = /usr/local/libexec/lcs/vip-post-start
# pre_stop = /usr/local/libexec/lcs/vip-pre-stop
# post_stop = /usr/local/libexec/lcs/vip-post-stop
```

Systemd service resources use `[service NAME]` sections:

```
[service app]
group = service
home_node = a
depends_on = vip1
systemd_unit = app.service
```

Similar config needs to exist on each node, only difference is IP to listen on and node name. That's all you need. Now launch lcsd on all nodes, it should form the quorum and set up resources. For troubleshooting use -vvv for maximal logs.

# Observability

lcs tool can be used to display cluster status, list resources, run simple monitoring checks, move resources from one node to another one, temporarily stop/start resources, or acknowledge conflicting states
```
# lcs status
Cluster
  quorum: yes (3 votes, need 2, membership for 2h 13m 04s)
  protocol: 1.1.0 (wire 5)
Nodes
  node1 role=full-member state=online (self)
  node2 role=full-member state=online
  node3 role=quorum-only state=online
Resources
  vip1 192.168.6.70/24 dev=enX0 state=active owner=node1 epoch=11 group=service priority=1
```

Use `--json` with CLI commands when integrating with automation:

```
# lcs --json status
{"cluster":{"quorum":true,"votes_seen":3,"quorum_needed":2,"membership_seconds":7984,"effective_protocol":5,"effective_protocol_release":"1.1.0"},"nodes":[{"id":0,"name":"node1","role":"full-member","state":"online","self":true}],"resources":[{"id":0,"name":"vip1","type":"vip","state":"active","owner":"node1","epoch":11,"lease_id":42,"address":"192.168.6.70/24","interface":"enX0","group":"service","priority":1,"disabled":false}]}

# lcs --json resource list
{"resources":[{"name":"vip1","type":"vip","state":"active","owner":"node1","address":"192.168.6.70/24","interface":"enX0","disabled":false}]}
```

For NRPE / Nagios-style monitoring:

```
# lcs nrpe
OK - quorum=yes votes=3/3 need=2 membership_for=2h 13m 04s nodes=3/3 resources=1/1 active
```

Exit codes are `0` for OK, `1` for WARNING when any node is offline, `2` for CRITICAL when quorum is lost or any resource is not active, and `3` if local status cannot be read.

Prometheus can be used to monitor cluster state

```
# curl localhost:9120/metrics
# HELP lcs_cluster_quorum Whether this node currently sees cluster quorum.
# TYPE lcs_cluster_quorum gauge
lcs_cluster_quorum{cluster="ingress"} 1
# TYPE lcs_cluster_votes_seen gauge
lcs_cluster_votes_seen{cluster="ingress"} 3
# TYPE lcs_cluster_votes_needed gauge
lcs_cluster_votes_needed{cluster="ingress"} 2
# HELP lcs_cluster_membership_seconds Seconds since this node's observed online/offline cluster membership last changed.
# TYPE lcs_cluster_membership_seconds gauge
lcs_cluster_membership_seconds{cluster="ingress"} 7984
# TYPE lcs_node_online gauge
# TYPE lcs_node_voting_ready gauge
lcs_node_online{cluster="ingress",node="node1",role="full-member"} 1
lcs_node_voting_ready{cluster="ingress",node="node1",role="full-member"} 1
lcs_node_online{cluster="ingress",node="node2",role="full-member"} 1
lcs_node_voting_ready{cluster="ingress",node="node2",role="full-member"} 1
lcs_node_online{cluster="ingress",node="node3",role="quorum-only"} 1
lcs_node_voting_ready{cluster="ingress",node="node3",role="quorum-only"} 1
# TYPE lcs_resource_state gauge
# TYPE lcs_resource_owner gauge
# TYPE lcs_resource_epoch gauge
# TYPE lcs_resource_lease_remaining_seconds gauge
# TYPE lcs_resource_conflict gauge
# TYPE lcs_resource_stop_failed gauge
# TYPE lcs_resource_failovers_total counter
# TYPE lcs_resource_priority gauge
lcs_resource_state{cluster="ingress",resource="vip1",type="vip",state="active"} 1
lcs_resource_owner{cluster="ingress",resource="vip1",type="vip",node="node1"} 1
lcs_resource_owner{cluster="ingress",resource="vip1",type="vip",node="node2"} 0
lcs_resource_owner{cluster="ingress",resource="vip1",type="vip",node="node3"} 0
lcs_resource_epoch{cluster="ingress",resource="vip1",type="vip"} 11
lcs_resource_lease_remaining_seconds{cluster="ingress",resource="vip1",type="vip"} 3.909
lcs_resource_conflict{cluster="ingress",resource="vip1",type="vip"} 0
lcs_resource_stop_failed{cluster="ingress",resource="vip1",type="vip"} 0
lcs_resource_failovers_total{cluster="ingress",resource="vip1",type="vip"} 1
lcs_resource_priority{cluster="ingress",resource="vip1",type="vip",group="service"} 1
```
