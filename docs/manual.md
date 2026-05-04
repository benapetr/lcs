# Node roles
All configured nodes are voting cluster members and participate in quorum, lease grants, lease renewal, and cluster state decisions. There is no dedicated arbiter authority or single lease authority.

full-member nodes can vote and can run resources such as VIPs.

quorum-only nodes can vote and participate in leases, but must never run resources or activate VIPs. They are intended for arbiter-style placement in small clusters, for example two full-member nodes plus one quorum-only node, but they are not special: any voting majority can grant or renew leases.

Every full-member is eligible to run every configured VIP. There is no per-resource eligible node list in the initial design.

Quorum is calculated from all configured voting members using floor(voting_members / 2) + 1. For a 3-node cluster, any 2 nodes form quorum: full-member + full-member, full-member + quorum-only, or quorum-only + full-member.

# Config
The config should use a strict INI-style format that is easy to parse in C: section headers, key = value pairs, blank lines, and comments starting with # or ;. Unknown keys, duplicate keys, malformed lines, missing required values, and invalid addresses should be errors.

The default cluster TCP port is 3322 and must be configurable. Lease duration, renew interval, peer timeout, conflict probe count, conflict probe timeout, and hook timeout must also be configurable, with defaults such as lease_ms = 5000, renew_ms = 1000, peer_timeout_ms = 5000, probe_count = 3, probe_timeout_ms = 300, and hook_timeout_ms = 5000.

Config sections and keys:

`[cluster]`
- `name`: required cluster name. Must match on all nodes.
- `node`: required local node name. Must match one configured `[node NAME]` section.
- `bind`: optional local daemon-to-daemon bind address. Default: empty, meaning all addresses.
- `socket`: optional local CLI Unix socket path. Default: `/run/lcs/lcsd.sock`.
- `pidfile`: optional pidfile path for manual `--daemonize` deployments. Default: empty, meaning no pidfile.
- `secret`: optional shared cluster secret checked during peer HELLO. Default: empty.
- `port`: optional default daemon-to-daemon TCP port. Default: `3322`.
- `lease_ms`: optional lease duration in milliseconds. Default: `5000`.
- `renew_ms`: optional local renew interval in milliseconds. Default: `1000`; must be lower than `lease_ms`.
- `peer_timeout_ms`: optional peer timeout in milliseconds. Default: `5000`.
- `probe_count`: optional ARP / Neighbor Discovery probe and announcement count. Default: `3`.
- `probe_timeout_ms`: optional per-probe conflict wait in milliseconds. Default: `300`.
- `hook_timeout_ms`: optional timeout for each VIP event hook in milliseconds. Default: `5000`.
- `syslog`: optional `true` / `false` switch for syslog logging. Default: `true`.
- `vip_backend`: optional VIP address management backend, either `ip` or `netlink`. Default: `ip`.
- `metrics`: optional `true` / `false` switch for the Prometheus metrics HTTP endpoint. Default: `true`.
- `metrics_bind`: optional metrics bind address. Default: `127.0.0.1`.
- `metrics_port`: optional metrics TCP port. Default: `9120`.

`[node NAME]`
- `role`: required, either `full-member` or `quorum-only`.
- `address`: required peer address, IPv4, IPv6, or hostname.
- `port`: optional peer TCP port override. Default: the cluster `port` value at the time the node section is declared.

`[vip NAME]`
- `address`: required VIP address in IPv4 or IPv6 CIDR form, for example `192.0.2.10/32` or `2001:db8::10/128`.
- `interface`: required Linux interface name used to add/remove and probe the VIP.
- `pre_start`: optional absolute path to run after the node obtains the majority lease but before conflict checks and VIP add.
- `post_start`: optional absolute path to run after successful VIP activation and announcement.
- `pre_stop`: optional absolute path to run before planned VIP removal.
- `post_stop`: optional absolute path to run after planned VIP removal.

VIP hooks are executed directly with `exec`, not through a shell, and receive context through environment variables: `LCS_CLUSTER`, `LCS_NODE`, `LCS_VIP`, `LCS_ADDRESS`, `LCS_INTERFACE`, `LCS_EVENT`, `LCS_EPOCH`, `LCS_LEASE_ID`, and `LCS_HOOK_TIMEOUT_MS`. Hook scripts must exit with status `0` to be considered successful. `pre_start` failure or timeout aborts activation and releases the lease. `pre_stop` failure or timeout is logged, but the VIP is still removed. Hook execution is asynchronous in the daemon event loop, so peer traffic and lease renewal continue while a hook is running. If quorum is lost or the local lease expires, lcsd bypasses stop hooks and removes the VIP immediately for safety.

# Local CLI
lcs communicates with the local lcsd through a Unix domain socket, defaulting to /run/lcs/lcsd.sock. CLI commands should not talk directly to the daemon-to-daemon cluster TCP port.

# Protocol
Daemon-to-daemon TCP and the local CLI socket should use the same minimal binary framing: a fixed header followed by a message-specific binary payload.

The frame header should include a magic value, protocol version, message type, payload length, and sequence number for request / response correlation. Message type should be a small integer enum, not a string. Payloads should use fixed-width integer fields and numeric node / resource IDs derived from the config. Multi-byte fields must use network byte order, and encode / decode helpers should be used instead of trusting compiler struct layout directly.

Frames must have a reasonable maximum size, for example 64 KiB, and invalid magic, unsupported version, unknown message type, or invalid length must close the connection cleanly.

Daemon peers should use persistent TCP connections for liveness, state convergence, lease grants, lease renewals, lease releases, and forwarded move requests rather than opening a fresh connection for every peer operation. The initial HELLO / HELLO_ACK payload identifies whether the connection is a short-lived recovery request socket or a persistent peer socket. To avoid duplicate persistent sessions, the lower configured node ID opens the outbound connection and the higher configured node ID accepts it. State sync messages may carry the sender's current volatile state so both sides can converge over the same persistent connection.

# Logging and daemon mode
lcsd should log to syslog by default. It should also support a command-line flag for foreground / stdout logging for debugging and service supervision.

Daemonization must be explicit with `--daemonize`; running `lcsd` without that flag must not fork into the background. Systemd units should run `lcsd --foreground --no-timestamp` as a simple service and should not require a pidfile. A pidfile may be configured for manual daemonized deployments.

Support --verbose to enable more detailed debug logs. Normal logs should cover state changes, quorum loss/restoration, lease grant/renew/release decisions, VIP add/remove operations, move requests, and conflict states.

# Security
Cluster traffic is plain TCP intended for a secure, dedicated VLAN. To keep the implementation lightweight, the initial design does not require TLS or cryptographic message signing.

The config may support an optional shared cluster secret for simple peer admission / accidental misconfiguration protection, but this is not a substitute for network isolation and should not make the wire protocol complex.

# Leases
Use a short-lived lease granted by majority quorum to control VIP ownership: only an eligible full-member node holding a valid lease may have the VIP, and it must continuously renew the lease with a majority of all configured voting members. If it loses majority quorum, it drops the VIP immediately. If a node crashes or freezes and can’t release cleanly, the lease simply expires after a few seconds, and only then another eligible full-member node is allowed to take over—preventing both resource-capable nodes from ever intentionally activating the VIP at the same time.

Lease authority is always the current majority, never one special node.

## Lease timing
Lease expiry and renewal scheduling must use each node's local monotonic clock. Nodes must not compare wall-clock timestamps from different machines to decide whether a lease is valid, because wall clocks can jump or drift.

Lease messages may carry durations and epochs, but validity is measured locally from the time a node accepted or renewed the lease. If the local monotonic timer says the lease expired, the node must treat it as expired and remove any VIP it owns for that lease.

# Resource epochs
Every resource must have a monotonically increasing epoch / generation number. Epochs are volatile cluster state kept in memory; lcsd should not require writing cluster state to disk.

Lease grants, lease renewals, ownership changes, and CLI move requests must include the resource epoch they apply to. Voting nodes must reject any lease claim, renewal, or move request with an epoch older than the newest epoch they have already accepted for that resource. A new owner or manual move must advance the resource epoch before ownership can change. This prevents stale packets, delayed lease grants, restarted daemons, or old CLI commands from reactivating an obsolete owner within the current cluster incarnation.

When lcsd starts, it must not activate any VIP immediately. It must first connect to peers, fetch the newest known resource state / epoch from the cluster, and adopt the highest epoch accepted by majority quorum. If majority quorum cannot be reached, the node cannot own VIPs.

Each daemon start should generate a fresh random node instance ID. Cluster messages should include node ID, node instance ID, resource epoch, and lease ID so nodes can reject stale messages from old daemon instances or old connections.

If the entire cluster restarts and all in-memory epochs are lost, the cluster may begin a new incarnation from initial epochs. This is allowed only because no node may keep a VIP active across daemon startup unless it reacquires a fresh majority lease. On startup, if lcsd finds a managed VIP configured locally but does not hold a current majority lease for it, it must remove that VIP before joining normal resource management.

# Manual VIP move:
The CLI move command must be a controlled cluster-level handoff, not a direct command to add a VIP on a target node.

When moving a VIP, lcs should request the move from lcsd, and the cluster must:
1. verify majority quorum is available
2. verify the target node is an eligible full-member for the VIP
3. advance the resource epoch for the move
4. ask the current owner to release the VIP and stop renewing the old lease
5. wait for the current owner to confirm the VIP was removed, or wait for the old lease to expire if the current owner is unreachable
6. grant a new majority lease for the advanced epoch to the target node
7. allow the target node to run the normal VIP activation safety checks before adding the VIP

The target node must never activate the VIP only because the CLI asked it to. It may activate only after it holds a current majority lease for the new epoch and the previous owner has either confirmed release or its old lease has expired.

# Automatic placement:
When the cluster starts with majority quorum and a VIP has no known owner, lcsd should automatically place it on the first eligible full-member by sorted node name, after advancing the resource epoch, acquiring a majority lease, and passing the normal VIP activation safety checks.

# VIP activation safety:
Before activating a VIP, the node must hold a valid quorum lease for that VIP and perform an additional same-L2 conflict check. Full-member nodes that can host the VIP are expected to be on the same L2 segment, so lcsd should ARP-probe IPv4 VIPs or use IPv6 Neighbor Discovery checks for IPv6 VIPs, optionally ICMP ping it, and refuse activation if another MAC/host appears to already own it. This check is an additional safety guard for stale local state, daemon restarts, or manual admin mistakes; it does not replace quorum leases.

IP conflict detection must block VIP activation, not merely log a warning. If conflict detection sees the VIP active elsewhere, even when quorum says this node should own it, the resource must enter a conflict state, lcsd must log the condition clearly, and the VIP must remain inactive on this node until an administrator inspects and resolves the situation.

The exact probe count and timeout should be configurable, with conservative defaults. The activation decision must be based on completed conflict checks, not a background best-effort probe. After successful activation, lcsd should announce the VIP with gratuitous ARP for IPv4 or unsolicited Neighbor Advertisement for IPv6.

The cluster must be operational whenever majority quorum is available. In the typical 3-node setup, that means any one node can be down, no node can become a SPOF, and if at least 2 nodes are up, cluster management must work.
