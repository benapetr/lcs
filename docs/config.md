# Configuration Reference

This is an annotated full configuration template. The same cluster definition
should be used on every node, with only the local `[cluster]` `node` value
changed per host. Lines starting with `#` or `;` are comments; inline comments
after values are also supported.

```ini
# /etc/lcs/lcs.conf
#
# LCS configuration is INI-like:
#   [cluster]       one required cluster section
#   [node NAME]     one required section per cluster member
#   [group NAME]    optional VIP placement group sections
#   [vip NAME]      one section per managed VIP resource
#
# Names may contain letters, numbers, "_", "-", ".", and ":".
# Boolean values may be true/false, yes/no, on/off, or 1/0.
# IPv4 and IPv6 addresses are both supported.

[cluster]
# REQUIRED: cluster name. Peers reject nodes from a different cluster.
name = example

# REQUIRED: local node name. Change this on each host to match one [node NAME].
node = node1

# OPTIONAL: local address used for daemon-to-daemon TCP.
# Default: empty, which binds on all local addresses.
bind = 192.0.2.11

# OPTIONAL: daemon-to-daemon TCP port.
# Default: 3322.
port = 3322

# OPTIONAL: local Unix socket path used by the lcs CLI.
# Default: /run/lcs/lcsd.sock.
socket = /run/lcs/lcsd.sock

# OPTIONAL: pidfile path used only when lcsd is started with --daemonize.
# Default: empty, which disables pidfile creation.
pidfile = /run/lcs/lcsd.pid

# OPTIONAL: shared cluster secret checked during peer handshake.
# Default: empty, which disables shared-secret checking.
# This is not transport encryption; keep the cluster network isolated.
secret = change-me-shared-secret

# OPTIONAL: majority lease duration in milliseconds.
# Default: 5000.
lease_ms = 5000

# OPTIONAL: lease renewal interval in milliseconds.
# Must be lower than lease_ms.
# Default: 1000.
renew_ms = 1000

# OPTIONAL: peer liveness timeout in milliseconds.
# Default: 5000.
peer_timeout_ms = 5000

# OPTIONAL: number of address-conflict probes before VIP activation.
# Default: 3.
probe_count = 3

# OPTIONAL: timeout for each conflict probe in milliseconds.
# Default: 300.
probe_timeout_ms = 300

# OPTIONAL: maximum runtime for each VIP hook in milliseconds.
# Must be greater than 0.
# Default: 5000.
hook_timeout_ms = 5000

# OPTIONAL: send daemon logs to syslog.
# Default: true.
syslog = true

# OPTIONAL: VIP address management backend.
# Allowed values: ip, netlink.
# Default: ip.
vip_backend = ip

# OPTIONAL: enable Prometheus metrics HTTP endpoint.
# Default: true.
metrics = true

# OPTIONAL: Prometheus metrics bind address.
# Default: 127.0.0.1.
metrics_bind = 127.0.0.1

# OPTIONAL: Prometheus metrics TCP port.
# Default: 9120.
metrics_port = 9120

[node node1]
# REQUIRED: node role.
# full-member can vote, hold leases, and run VIP resources.
# quorum-only can vote and hold lease state but never activates VIPs.
role = full-member

# REQUIRED: peer address for daemon-to-daemon TCP.
address = 192.0.2.11

# OPTIONAL: per-node peer TCP port.
# Default: the [cluster] port value.
port = 3322

[node node2]
role = full-member
address = 192.0.2.12
port = 3322

[node node3]
role = quorum-only
address = 192.0.2.13
port = 3322

[group public]
# REQUIRED for group sections: placement rule type.
# keep-together keeps grouped VIPs on the same full-member when possible.
# anti-affinity spreads grouped VIPs across different full-members when possible.
type = keep-together

# REQUIRED for group sections: enforcement mode.
# strict may leave lower-priority VIPs stopped if the rule cannot be satisfied.
# best-effort places VIPs anyway, then rebalances when the rule can be satisfied.
mode = best-effort

[vip public_v4]
# REQUIRED: VIP address in IPv4 or IPv6 CIDR form.
address = 198.51.100.50/24

# REQUIRED: Linux interface used for this VIP.
# If copied from "ip addr", display names like bond1.3675@bond1 are accepted
# and normalized to the kernel interface name before "@", for example bond1.3675.
interface = eth0

# OPTIONAL: placement group name. Must match a [group NAME] section.
# Default: empty, meaning this VIP is not grouped.
group = public

# OPTIONAL: group priority. Higher number means higher priority.
# Priorities must be unique within one group.
# Default: derived from sorted VIP order, with earlier VIPs getting higher priority.
priority = 200

# OPTIONAL: hook run after majority lease acquisition, before conflict probing and VIP add.
# Path must be empty or absolute.
# Default: empty.
pre_start = /usr/local/libexec/lcs/public-pre-start

# OPTIONAL: hook run after successful VIP activation.
# Path must be empty or absolute.
# Default: empty.
post_start = /usr/local/libexec/lcs/public-post-start

# OPTIONAL: hook run before planned VIP removal.
# Path must be empty or absolute.
# Default: empty.
pre_stop = /usr/local/libexec/lcs/public-pre-stop

# OPTIONAL: hook run after planned VIP removal.
# Path must be empty or absolute.
# Default: empty.
post_stop = /usr/local/libexec/lcs/public-post-stop

[vip public_v6]
address = 2001:db8:100::50/64
interface = eth0
group = public
priority = 100

# OPTIONAL: hooks may be omitted per VIP by leaving these keys out.
# If omitted, no hook is executed for that event.
```
