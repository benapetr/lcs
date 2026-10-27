# Changelog

## Next

- Fixed final peer replies being discarded when readable data and connection closure arrive in the same epoll event, including committed configuration proofs needed by stale nodes.

- Added the local durable application catalog for future dynamic configuration, retaining old and pending backend definitions through cleanup/restart and planning incarnation/dependency drains. It is not yet connected to the daemon resource coordinator.

- Added schema-2 resource incarnation snapshots and successor validation for the future dynamic authority, including deletion/recreation and backend replacement identities persisted through agreement restart. Live activation and runtime message gating remain pending; legacy bootstrap still uses schema 1.

- Added the durable configuration-agreement core, majority proposal tracking, background storage workers, and bounded snapshot-transfer codecs. These are not yet connected to live peer transactions or activation; INI remains authoritative and wire protocol remains version 5. Agreement hashing adds an OpenSSL libcrypto build dependency.
- Added the JSON resource snapshot and durable local bootstrap foundation for distributed configuration, including automatic preparation, background refresh after INI reload, and state-directory packaging. INI remains authoritative and runtime definition commands are not enabled yet; wire protocol remains version 5. Building the daemon now requires Jansson development files.
- Added a validated internal candidate editor for future pcs-style resource and group commands, including affinity policies and revision checks. This is not yet exposed through the runtime CLI.
- Added optional `peers_only` source checking, disabled by default. When enabled, every node requires an explicit numeric IP address, incoming sources must match configured peers, and HELLO identities must match their source IPs. It works alongside `allow_from` and requires daemon restart to change.
- Added an incoming peer source-IP allowlist. Defaults accept RFC 1918 private IPv4 ranges only; configure `allow_from` for localhost, IPv6, or public peer networks, or set `0.0.0.0/0` to disable filtering for both address families. Disallowed sources are rejected before HELLO processing. Policy changes require daemon restart; wire protocol remains version 5.

## v1.1.1

- Fixed a reconnect loop after failed resource acquisition: tentative epochs remain private voter state, valid ownership observations can recover without weakening voter promises, and stale lease announcements no longer disconnect peers. Lease and state broadcasts now wait for the peer handshake to complete.
- Added terminal colors to `lcs status` and `lcs resource list`: green for healthy states, red for unavailable or failed states, and yellow for transitional or administratively stopped states. Use `--no-colors` to disable colors. Redirected output, JSON, and `nrpe` remain plain.
- Added deterministic lease and handshake regression coverage, a grouped IPv4/IPv6 rolling-restart integration test, and unit-test execution in CI.
- Peer wire protocol remains version 5, compatible with v1.1.0.

## v1.1.0

### Highlights
- Added dynamic configuration reload support and improved handling for rolling upgrades.
- Made resource backend operations asynchronous, which reduced blocking during VIP probing and worker cancellation.
- Introduced systemd-oriented worker integration and improved resource lifecycle handling.
- Added and refined resource management features, including new resource types, dependencies, and better move behavior.
- Strengthened cluster safety and recovery: startup consistency checks, node quarantine after restart, improved brain-split handling, and more robust state synchronization.
- Added a new CLI server interface with JSON output, plus expanded configuration, packaging, and documentation.

### Notable changes
- Config reload and startup validation now verify consistency before the cluster becomes active.
- Managed resources are stopped cleanly on startup and removed properly on shutdown.
- Resource moves are faster and more reliable, and quorum grant handling was added.
- VIP and resource operations are now handled asynchronously, improving responsiveness and reducing deadlock risk.
- New examples and docs were added for systemd service deployment, configuration, and cluster setup.

## v1.0.4

### Highlights
- Added scheduler-related improvements and cleanup work.
- Expanded example clusters and documentation for grouped and localhost-based deployments.
- Added NRPE support and related integration coverage.
- Improved logging and signal handling for better operational visibility.

### Notable changes
- The daemon and peer handling were adjusted to better support cluster operation and lifecycle behavior.
- The resource and scheduler layers were refined to improve control flow and coordination.
- New hook examples and config validation coverage were added.

## v1.0.3

### Highlights
- Improved packaging and RPM support.
- Added quorum age handling and related reliability improvements.
- Cleaned up redundant logging and adjusted protocol behavior.

### Notable changes
- Packaging scripts and service definitions were updated for smoother installation and operation.
- Cluster and lease logic saw small but meaningful robustness improvements.
- Utility support was added to better handle shared helper behavior.

## v1.0.2

### Highlights
- Added resource grouping support, including grouped resource movement and anti-affinity behavior.
- Introduced better config parsing and validation for grouped resources.
- Added comprehensive integration tests around groups, hooks, and cluster behavior.
- Expanded example configurations to demonstrate simple, grouped, and IPv6 clusters.

### Notable changes
- Grouped resources can now move together or obey anti-affinity constraints as configured.
- Hooks and resource management behavior were refined.
- Cluster, lease, move, and peer logic were adjusted to support the new grouping semantics.

## v1.0.1

### Highlights
- Major refactor of core cluster and lease logic.
- Added a new move engine and scheduler support.
- Expanded integration coverage for failover, remote moves, and restart scenarios.
- Improved local client and resource handling.

### Notable changes
- The internal architecture was reorganized to make resource movement and scheduling more structured.
- Failover and lease coordination behavior became more testable and robust.
- Metrics, logging, and peer communication were adjusted as part of the refactor.

## v1.0.0

### Highlights
- Initial public release of the project.
- Introduced the core clustering model: leases, peer communication, VIP/resource management, local client access, metrics, and basic daemon behavior.
- Included early integration tests and packaging support.

### Notable changes
- Established the baseline architecture for cluster membership, resource ownership, and lease handling.
- Added the initial command-line and daemon components used throughout later releases.
