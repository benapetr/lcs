# Configuration Reference

This is an annotated full configuration template. The same cluster definition
should be used on every node, with only the local `[cluster]` `node` value
changed per host. Lines starting with `#` or `;` are comments; inline comments
after values are also supported.

## Live resource reload

Deploy the edited configuration file to every currently online node, then run
`lcs reload` or `systemctl reload lcsd` on any one node (or send `SIGHUP` to
one `lcsd`). The receiving node captures itself and every currently connected
peer as a fixed participant set. That set must contain quorum, at least one
full member, and every participant must support resource reload. Members already offline do not
participate or block the change. Every participant loads its local file
automatically and must report a compatible candidate, then acknowledge the
same complete participant set before any resource is touched. Only after that
agreement barrier does the cluster stop
resources being removed or whose VIP address/interface, resource type, or
systemd unit changed, followed by the commit.

If a captured participant disconnects before commit, the attempt aborts. Retry
the reload to capture the new online set. A member excluded because it was
offline cannot rejoin with the old resource configuration. Its handshake gives
an actionable mismatch and records a non-voting commit proof. Deploy the
committed file there and run `lcs reload`; the node drains its old local
resources, adopts the committed schema, and rejoins through recovery without a
restart. If an excluded full member could still hold a changed resource,
the transaction waits one complete lease interval before commit.
Deploy the same committed file to every excluded node before bringing it back.

Every reload request also refreshes cached peer hostnames asynchronously. A
successful lookup with usable addresses replaces that peer's cache for future
connection attempts. A failed or empty lookup retains the previous cache, and
DNS changes never disconnect an established peer. This refresh does not make
the configured node `address` itself live-reloadable.

Resources and groups may be added, removed, or modified. Unchanged resources
retain ownership, lease, epoch, and administrative state. A changed backend is
drained and reintroduced as a stopped resource before normal placement resumes.
Invalid or mismatched candidates do not drain resources or commit; correct the
files and reload again. A node locks its candidate when it acknowledges that
the complete candidate set matches; later reload signals on that node are
ignored until commit. This prevents a file edit from invalidating an agreement
that another node may already be using. Before agreement, restoring the active
resource configuration and reloading withdraws that node's pending candidate;
unchanged resource configuration is treated as a no-op.

Node membership, node roles and addresses, cluster timing, listeners, metrics,
authentication, and other daemon-wide settings are deliberately rejected by
live resource reload. Live membership changes require joint-consensus quorum
transitions and are not implemented yet.

Do not use `systemctl restart lcsd` as a substitute for reload after editing
resource configuration. A restarted daemon has forgotten the previously active
configuration and treats the edited file as active immediately, so existing
members reject it as a configuration mismatch. Reload and wait for the
candidate to commit before restarting any daemon.

```ini
# /etc/lcs/lcs.conf
#
# LCS configuration is INI-like:
#   [cluster]       one required cluster section
#   [node NAME]     one required section per cluster member
#   [group NAME]    optional resource placement group sections
#   [vip NAME]      one section per managed VIP resource
#   [service NAME]  one section per managed systemd service resource
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
# Default: netlink.
vip_backend = netlink

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

# OPTIONAL: peer address for daemon-to-daemon TCP. It may be an IPv4 address,
# IPv6 address, or hostname. If omitted, the node section name is resolved.
# An explicit address takes precedence over the node name.
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

# OPTIONAL: preferred full-member node for this VIP.
# Manual moves away from the home node block automatic return until the
# resource is manually moved back home.
# Default: empty, meaning this VIP keeps current placement unless normal failover
# or group rebalance moves it.
home_node = node-a

# OPTIONAL: comma-separated hard resource dependencies.
# This resource starts only after all dependencies are active on the same node.
# Dependents stop before this resource stops.
# depends_on = base-vip, storage-vip

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

[service app]
# REQUIRED: systemd unit name managed through D-Bus.
systemd_unit = app.service

# OPTIONAL: service resources use the same placement controls and hooks as VIPs.
group = public
priority = 50
home_node = node-a
depends_on = public_v4
# pre_start = /usr/local/libexec/lcs/app-pre-start
# post_start = /usr/local/libexec/lcs/app-post-start
# pre_stop = /usr/local/libexec/lcs/app-pre-stop
# post_stop = /usr/local/libexec/lcs/app-post-stop
```
