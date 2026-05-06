# LCS — Lease-based Cluster Service

LCS is a lightweight high-availability daemon that manages virtual IP addresses (VIPs) across a cluster of Linux nodes. Ownership of each VIP is controlled by a short-lived lease that must be continuously renewed by a majority of voting nodes. When a node loses quorum or crashes, the lease expires and another eligible node takes over.

`lcsd` is the cluster daemon. `lcs` is the CLI used to query and control the local daemon.

---

# Node roles

Every node listed in the configuration is a voting cluster member. All voting members participate in quorum, lease grants, lease renewal, and cluster-state decisions. There is no dedicated arbiter and no single lease authority.

**full-member** — can vote and can run resources such as VIPs. Every full-member is eligible to host every configured VIP.

**quorum-only** — can vote and participates in leases, but never runs resources or activates VIPs. This role is intended for arbiter-style placement in small clusters—for example, two full-member nodes plus one quorum-only node—without requiring a third full server.

Quorum is calculated as `floor(voting_members / 2) + 1`. In a 3-node cluster any two nodes form quorum, regardless of their roles.

---

# Configuration

Configuration uses an INI-style file. The same format is used on every node; only the `node` key in the `[cluster]` section differs per host. Comments starting with `#` or `;` may be whole-line comments or inline comments after values.

## `[cluster]`

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `name` | yes | — | Cluster name. Must be identical on all nodes. |
| `node` | yes | — | Local node name. Must match one `[node NAME]` section. |
| `bind` | no | *(all addresses)* | Local bind address for daemon-to-daemon TCP. |
| `socket` | no | `/run/lcs/lcsd.sock` | Unix socket path for the local CLI. |
| `pidfile` | no | *(none)* | Pidfile path, used only with `--daemonize`. |
| `secret` | no | *(none)* | Shared cluster secret verified during peer handshake. |
| `port` | no | `3322` | Default daemon-to-daemon TCP port. |
| `lease_ms` | no | `5000` | Lease duration in milliseconds. |
| `renew_ms` | no | `1000` | Lease renewal interval in milliseconds. Must be less than `lease_ms`. |
| `peer_timeout_ms` | no | `5000` | Time in milliseconds before an unresponsive peer is considered down. |
| `probe_count` | no | `3` | Number of ARP / Neighbor Discovery probes and announcements. |
| `probe_timeout_ms` | no | `300` | Per-probe conflict-detection wait in milliseconds. |
| `hook_timeout_ms` | no | `5000` | Maximum time in milliseconds allowed for each VIP hook to complete. |
| `syslog` | no | `true` | Send log output to syslog. |
| `vip_backend` | no | `ip` | VIP address management backend: `ip` or `netlink`. |
| `metrics` | no | `true` | Enable the Prometheus metrics HTTP endpoint. |
| `metrics_bind` | no | `127.0.0.1` | Bind address for the metrics endpoint. |
| `metrics_port` | no | `9120` | TCP port for the metrics endpoint. |

## `[node NAME]`

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `role` | yes | — | `full-member` or `quorum-only`. |
| `address` | yes | — | Peer address: IPv4, IPv6, or hostname. |
| `port` | no | cluster `port` | TCP port override for this peer. |

## `[group NAME]`

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `type` | yes | — | `keep-together` or `anti-affinity`. |
| `mode` | yes | — | `strict` or `best-effort`. |

`keep-together` prefers all VIPs in the group on the same full-member. In `strict` mode, lower-priority VIPs remain down if they cannot be placed with the active group owner. In `best-effort` mode, LCS prefers co-location but may temporarily place VIPs independently.

`anti-affinity` prefers VIPs in the group on different full-members. In `strict` mode, lower-priority VIPs remain down when there are not enough online full-members. In `best-effort` mode, LCS prefers separation but may place multiple VIPs on the same node if needed.

Group placement is enforced during automatic VIP placement and during periodic rebalance. Rebalance is deterministic: only the first online full-member by sorted node name starts automatic group moves, and only one rebalance move runs at a time. Automatic rebalance uses the same controlled move machinery as `lcs move`, so the old owner releases first, the cluster records a newer lease epoch, and the target activates only after it obtains a majority lease.

For best-effort anti-affinity, if multiple group VIPs temporarily run on one node because too few full-members were online, LCS moves lower-priority VIPs away when another full-member becomes available. For keep-together, if grouped VIPs end up split across full-members, LCS moves lower-priority VIPs to the active owner of the highest-priority grouped VIP.

For `keep-together` groups, a manual `lcs move` request for any group member is redirected to the highest-priority member first. This prevents a lower-priority follower from being moved away and then immediately moved back by rebalance.

## `[vip NAME]`

| Key | Required | Default | Description |
|-----|----------|---------|-------------|
| `address` | yes | — | VIP address in CIDR notation, e.g. `192.0.2.10/32` or `2001:db8::10/128`. |
| `interface` | yes | — | Linux interface on which the VIP is added and probed. |
| `group` | no | — | Group name from a `[group NAME]` section. |
| `priority` | no | inverse sorted VIP index | Positive integer priority inside the group. Higher numbers are higher priority. Priorities must be unique within a group. |
| `pre_start` | no | — | Absolute path to a script run after the lease is obtained but before the conflict check and VIP add. |
| `post_start` | no | — | Absolute path to a script run after the VIP is activated and announced. |
| `pre_stop` | no | — | Absolute path to a script run before planned VIP removal. |
| `post_stop` | no | — | Absolute path to a script run after planned VIP removal. |

### VIP hooks

Hook scripts are executed directly with `exec`, not through a shell. The following environment variables are provided to each hook:

`LCS_CLUSTER`, `LCS_NODE`, `LCS_VIP`, `LCS_ADDRESS`, `LCS_INTERFACE`, `LCS_EVENT`, `LCS_EPOCH`, `LCS_LEASE_ID`, `LCS_HOOK_TIMEOUT_MS`

A hook must exit with status `0` to be considered successful. If `pre_start` fails or times out, activation is aborted and the lease is released. If `pre_stop` fails or times out, the failure is logged but the VIP is still removed. Hook execution runs asynchronously in the daemon event loop, so peer communication and lease renewal continue while a hook is running. If quorum is lost or the local lease expires during a hook, `lcsd` skips any remaining stop hooks and removes the VIP immediately.

---

# Running the daemon

## Systemd (recommended)

The package installs a systemd unit that runs `lcsd --foreground --no-timestamp` as a simple service. No pidfile is required. Enable and start the service with:

```
systemctl enable --now lcsd
```

## Foreground / debugging

Running `lcsd` without `--daemonize` keeps the process in the foreground and writes logs to stdout:

```
lcsd --foreground --config /etc/lcs/lcs.conf
```

Use `--verbose` for detailed debug output covering state changes, quorum events, lease decisions, VIP operations, and conflict detection.

## Daemonized

```
lcsd --daemonize --config /etc/lcs/lcs.conf
```

A `pidfile` path must be set in the configuration when running in this mode.

---

# Local CLI

`lcs` communicates with the local `lcsd` through the configured Unix domain socket (default `/run/lcs/lcsd.sock`). All `lcs` commands target the local daemon and do not connect directly to cluster TCP ports on other nodes.

---

# Logging

`lcsd` logs to syslog by default. Pass `--foreground` to write to stdout instead. Logged events include:

- Daemon startup and shutdown
- Quorum gained and lost
- Lease grant, renewal, and release decisions
- VIP activation and removal
- Move requests
- Conflict detection results
- Peer connection state changes

---

# Security

Cluster traffic runs over plain TCP and is intended for a dedicated, isolated management network or VLAN. There is no TLS or per-message cryptographic signing.

The optional `secret` key in `[cluster]` enables a shared secret that is verified during the peer handshake. This protects against accidental cross-cluster connections and misconfiguration, but is not a substitute for proper network isolation.

---

# Leases

VIP ownership is controlled by a short-lived lease granted and renewed by majority quorum. Only a full-member node that holds a valid lease may activate a VIP. The owning node continuously renews the lease with a majority of all voting members. If it loses quorum, it drops the VIP immediately.

If a node crashes or becomes unresponsive without releasing the lease cleanly, the lease expires after `lease_ms` milliseconds. Only after expiry can another eligible full-member acquire a new lease and take over the VIP. This ensures no two nodes intentionally activate the same VIP simultaneously.

Lease authority is always the current voting majority. There is no designated lease master.

## Lease timing

Lease validity is measured against each node's local monotonic clock. Wall-clock timestamps from different machines are never compared. Durations and epochs are carried in lease messages, but a node's lease is valid only as long as its own monotonic timer has not passed the accepted expiry. If the local timer indicates expiry, the node treats the lease as expired and removes any associated VIP.

---

# Resource epochs

Each resource carries a monotonically increasing epoch number held in memory. Epochs are not persisted to disk.

Every lease grant, renewal, ownership change, and move request includes the epoch it applies to. Nodes reject any claim, renewal, or move request referencing an epoch older than the highest epoch they have already accepted for that resource. This prevents stale packets, delayed messages, or restarted daemons from reactivating an obsolete owner.

On startup, `lcsd` does not activate any VIP immediately. It first connects to peers, retrieves the current resource state and epochs, and adopts the highest epoch accepted by majority quorum. If majority quorum is unavailable, the node does not own any VIPs.

Each daemon start generates a fresh random instance ID. Peers use the combination of node ID, instance ID, epoch, and lease ID to detect and discard stale messages from old daemon instances or lingering connections.

If the entire cluster restarts and all in-memory epochs are lost, the cluster begins a new incarnation from baseline epochs. This is safe because no node retains a VIP across a daemon restart: on startup, if `lcsd` finds a locally configured VIP active on an interface but does not hold a current majority lease for it, the VIP is removed before normal operation begins.

---

# Moving a VIP

The `lcs move` command performs a controlled cluster-level handoff. It does not directly add the VIP on the target node.

When a move is requested, the cluster:

1. Verifies majority quorum is available.
2. Verifies the target node is an eligible full-member for the VIP.
3. Advances the resource epoch.
4. Instructs the current owner to release the VIP and stop renewing the old lease.
5. Waits for the current owner to confirm removal, or waits for the old lease to expire if the owner is unreachable.
6. Grants a new majority lease for the advanced epoch to the target node.
7. Allows the target node to complete the normal VIP activation safety checks before adding the VIP.

The target node activates the VIP only after it holds a current majority lease for the new epoch and the previous owner has either confirmed release or its old lease has expired.

---

# Automatic VIP placement

When the cluster reaches majority quorum and a VIP has no known owner, `lcsd` automatically places it on the first eligible full-member ordered by sorted node name. Placement follows the normal sequence: advance the epoch, acquire a majority lease, run the conflict check, then add the VIP.

Grouped VIPs may override this default target selection:

- `keep-together` chooses an existing active group owner when possible.
- `anti-affinity` chooses an online full-member that does not already host another active member of the same group when possible.
- `strict` mode may leave lower-priority VIPs stopped until the rule can be satisfied.
- `best-effort` mode may temporarily violate the rule to keep VIPs available, then rebalance when topology allows.

---

# VIP activation safety

Before activating a VIP, a node must hold a valid majority lease and pass a same-L2 conflict check. `lcsd` sends ARP probes for IPv4 VIPs and Neighbor Discovery probes for IPv6 VIPs to detect whether another host on the segment already owns the address.

If a conflict is detected—even when quorum indicates this node should own the VIP—activation is blocked. The resource enters a conflict state, `lcsd` logs the condition, and the VIP remains inactive until an administrator investigates and resolves the conflict manually.

Conflict detection is synchronous: the activation decision waits for all probes to complete. The probe count and per-probe timeout are controlled by `probe_count` and `probe_timeout_ms` in the configuration.

After successful activation, `lcsd` announces the VIP with a gratuitous ARP reply (IPv4) or an unsolicited Neighbor Advertisement (IPv6).

---

# Availability

The cluster remains operational whenever majority quorum is available. In a standard 3-node deployment any single node can be down and the cluster continues to manage VIPs normally. No single node is a point of failure.
