# Configuration Reference

This is an annotated full configuration template. The same cluster definition
should be used on every node, with only the local `[cluster]` `node` value
changed per host. Lines starting with `#` or `;` are comments; inline comments
after values are also supported.

## Prepared resource JSON

The daemon automatically prepares a local JSON resource snapshot and initialization
metadata on startup and refreshes the snapshot after successful resource reloads.
Normal upgrades retain INI authority; supported already-activated catalogs resume
JSON authority through fenced recovery. See
[resource JSON and storage](resource-json.md) for paths, permissions, recovery
behavior, and the schema. `lcs config status` (also with `--json`) reports
prepared snapshots and matching members; these observations do not activate
dynamic management. For activated catalogs, it also
reports local committed/applied/pending revisions and digests, recovery/admission
state and cleanup errors. These read-only observations do not authorize edits. Admitted JSON full members
with healthy request history support [runtime definition commands](resource-json.md#runtime-definition-commands)
through the local Unix socket; automatic upgrade activation remains unfinished.
New coordinated format-2 initialization makes request history mandatory before
preparation: missing/corrupt history after that point requires recovery, never
empty-journal recreation. Older format-1 catalogs keep their existing read-only
compatibility. See [required initialization history](resource-json.md#required-request-history-during-initialization).

A recovered format-3 freeze uses its retained resource/group definitions despite
later valid INI resource edits. It opens wire-6 coordination and read-only CLI
access with resource voting/placement, reload and mutations fenced. `config status`
reports `application_state: frozen`. Existing freezes resume local initialization
after fresh all-member confirmation, while pristine prepared restarts preserve
their journals and fences. Automatic first-freeze creation and JSON activation
remain unfinished. INI syntax/static validation still runs before recovery.

After local preparation of an existing durable freeze, the scheduler automatically
collects readiness from every configured member and persists its barrier.
`config status` can report `all-member readiness persisted; awaiting activation`
while retaining frozen INI authority and resource/mutation fences. A valid barrier
over revision-zero history also permits recovery of retained promises, request
reservations and an accepted activation value with its exact request identity,
without rewriting records or inferring commitment. In-memory
collection timeouts can retry; uncertain persistence requires disk recovery.
A learned revision-one activation can resume initial application after restart
or on a running frozen node without restarting,
using retained baseline cleanup, old-owner proof and fresh sync/quorum before
admission. During baseline cleanup, local diagnostic format 5 reports JSON
authority with committed revision 1, applied revision 0 and admission disabled;
cleanup failures remain visible as blocked. After an existing durable freeze
reaches readiness, a full member automatically
proposes the unchanged baseline's schema-2 activation through majority agreement.
Retries use fresh request IDs and higher ballots, preserving unknown reservations
and recovering accepted values. Quorum-only members never initiate proposals.
Ordinary INI startup still awaits automatic first-freeze initiation.

An explicit durable freeze of a running INI generation now fences new resource
work and drains owned backends with the retained hooks and dependency order.
FROZEN acknowledgements and local initialization wait for cleanup, workers and
lease state to drain. Failed cleanup remains frozen and is reported by
`config status`; repair/restart retains the baseline. Quorum-only nodes execute
no backends. Automatic first-freeze initiation from ordinary INI startup remains
unfinished. See [the live drain contract](dynamic-config-agreement.md#draining-a-running-ini-generation-after-freezing).

`lcs config request-status ID` (also with `--json`) reads published local request
facts during normal operation or recovery. Missing/poisoned history and unrecorded
outcomes remain unknown; a committed fact does not establish application or final
client success. An `applied` fact additionally records completed local admission,
not current resource health or acknowledgements from every peer. The query does
not initialize files or retry requests. See
[request facts](resource-json.md#inspecting-a-requests-local-facts).
Mutation reasons expose exact blocked-attempt cleanup/projection errors and
storage-recovery instructions without changing pending/unknown outcomes. Fatal
shutdown makes a nonblocking best-effort diagnostic attempt; a lost connection
still means unknown. See [failure diagnostics](resource-json.md#mutation-failure-diagnostics).
`lcs config import FILE|-` submits one complete resource/group replacement through
the same guarded transaction path; it cannot import static daemon/node settings.
Source revision/identity metadata is validated but reallocated against the current
committed predecessor. See [atomic bulk import](resource-json.md#atomic-bulk-import)
for expected-base checks, deletion semantics, input limits and exact-byte retries.
`lcs config export [committed|applied]` writes the corresponding resource/group
JSON document from running memory. With INI authority both views describe effective
INI definitions at revision zero. See [export details](resource-json.md#exporting-definitions).
`lcs --config /path/node.conf config validate FILE` validates JSON offline using
that INI file as the static cluster/node context. Use `-` for stdin and `--json`
for structured results. It neither contacts a daemon nor changes files/resources;
see [validation details](resource-json.md#validating-a-document-offline).

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

## Hook execution

The `pre_start`, `post_start`, `pre_stop` and `post_stop` programs must run in the
foreground and wait for child commands to finish. Their process group is
cancelled on timeout, cancellation or daemon exit. Stop hooks should be idempotent
so cleanup can repeat safely. See the [hook environment](manual.md#vip-hooks).
The explicit future dynamic startup path also supplies recovery context; it does
not currently enable automatic distributed recovery. See
[startup cleanup](dynamic-config-application.md#explicit-startup-backend-cleanup).

## Full template

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

# OPTIONAL: source networks accepted on the peer TCP listener.
# Default: 10.0.0.0/8,172.16.0.0/12,192.168.0.0/16 (RFC 1918 IPv4 only).
# An explicit list replaces the defaults; this example permits its peer subnet.
# Add loopback or IPv6 peer networks explicitly when needed.
# Use 0.0.0.0/0 to disable filtering for BOTH IPv4 and IPv6.
# Maximum 16 CIDRs / 255 characters. Changes require a daemon restart.
allow_from = 192.0.2.0/24

# OPTIONAL: additionally restrict incoming connections to configured peer IPs.
# Default: false. When true, EVERY node (including this node and quorum-only
# nodes) must explicitly set address to a numeric IPv4 or IPv6 address.
# Hostnames and omitted addresses are rejected; DNS is not used for this check.
# Both allow_from and peers_only must permit the source. 0.0.0.0/0 disables
# only allow_from, not peers_only. HELLO's claimed node must match its source IP.
# Configure the actual source IP seen by peers (routing/NAT can change it).
# Changes require a daemon restart.
peers_only = false

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
# With peers_only = true, every node must explicitly set a numeric IP here.
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
