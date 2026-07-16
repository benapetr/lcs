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

Concepts:
- Only a trivial config file that defines all cluster members, resource groups and resources (VIPs and systemd services; see examples)
- Listens only on specified interfaces / IPs (or any by default)
- lcsd uses TCP for daemon-to-daemon cluster communication
- Supports both IPv4 and IPv6 for cluster node addresses and VIP resources

# Building

Just run `make`.

Systemd service resources use D-Bus through sd-bus. Build with `make WITH_SYSTEMD=1` on systems with `libsystemd` development headers installed to enable service start/stop/health operations.

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
systemd_unit = app.service
```

The service unit should not be enabled to start independently on every node; LCS should be the actor that starts and stops it.

Similar config needs to exist on each node, only difference is IP to listen on and node name. That's all you need. Now launch lcsd on all nodes, it should form the quorum and set up resources. For troubleshooting use -vvv for maximal logs.

# Resource groups

Resources can be assigned to optional groups:

- `keep-together` prefers grouped resources on the same full-member.
- `anti-affinity` prefers grouped resources on different full-members.
- `strict` mode may keep lower-priority resources stopped when the rule cannot be satisfied.
- `best-effort` mode keeps resources available if possible, then rebalances when topology allows.

Priority is local to each group. Higher numbers are higher priority. If a priority is not set, LCS derives it from inverse sorted resource order, so earlier sorted resources get higher default priority.

Automatic rebalance uses the same safe move path as `lcs resource move`: the old owner releases first, quorum records a newer lease epoch, and the target activates only after it obtains a majority lease. Only one automatic rebalance move runs at a time.

For `keep-together` groups, moving any member through `lcs resource move` moves the highest-priority group member first. Other group members then follow through normal rebalance.

# Home nodes

Each resource can optionally set `home_node` to the name of a full-member node. When configured and not blocked, LCS places the resource on that node whenever it is online. Resources without a home node keep the existing behavior: after placement or failover, they remain wherever they last landed unless group rebalance or another failover moves them.

Manual `lcs resource move RESOURCE NODE` to a non-home node blocks automatic return for that resource. A later manual move back to the configured home node clears the block.

# Resource control

Use `lcs resource list` for a compact resource-only view. `lcs resource stop RESOURCE` marks a resource administratively stopped in cluster memory and releases it through the normal stop path, including hooks. It stays stopped until `lcs resource start RESOURCE` is called, or until the whole cluster is restarted.

# Observability

lcs tool can be used to display cluster status, list resources, run simple monitoring checks, move resources from one node to another one, temporarily stop/start resources, or acknowledge conflicting states
```
# lcs status
Cluster
  quorum: yes (3 votes, need 2, membership for 2h 13m 04s)
Nodes
  node1 role=full-member online=yes (self)
  node2 role=full-member online=yes
  node3 role=quorum-only online=yes
VIPs
  vip1 192.168.6.70/24 dev=enX0 state=active owner=node1 epoch=11 group=service priority=1
```

Use `--json` with CLI commands when integrating with automation:

```
# lcs --json status
{"cluster":{"quorum":true,"votes_seen":3,"quorum_needed":2,"membership_seconds":7984},"nodes":[{"id":0,"name":"node1","role":"full-member","online":true,"self":true}],"resources":[{"id":0,"name":"vip1","type":"vip","state":"active","owner":"node1","epoch":11,"lease_id":42,"address":"192.168.6.70/24","interface":"enX0","group":"service","priority":1,"disabled":false}]}

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
lcs_node_online{cluster="ingress",node="node1",role="full-member"} 1
lcs_node_online{cluster="ingress",node="node2",role="full-member"} 1
lcs_node_online{cluster="ingress",node="node3",role="quorum-only"} 1
# TYPE lcs_vip_state gauge
# TYPE lcs_vip_owner gauge
# TYPE lcs_vip_epoch gauge
# TYPE lcs_vip_lease_remaining_seconds gauge
# TYPE lcs_vip_conflict gauge
# TYPE lcs_vip_failovers_total counter
# TYPE lcs_vip_priority gauge
lcs_vip_state{cluster="ingress",vip="vip1",state="active"} 1
lcs_vip_owner{cluster="ingress",vip="vip1",node="node1"} 1
lcs_vip_owner{cluster="ingress",vip="vip1",node="node2"} 0
lcs_vip_owner{cluster="ingress",vip="vip1",node="node3"} 0
lcs_vip_epoch{cluster="ingress",vip="vip1"} 11
lcs_vip_lease_remaining_seconds{cluster="ingress",vip="vip1"} 3.909
lcs_vip_conflict{cluster="ingress",vip="vip1"} 0
lcs_vip_failovers_total{cluster="ingress",vip="vip1"} 1
lcs_vip_priority{cluster="ingress",vip="vip1",group="service"} 1
```
