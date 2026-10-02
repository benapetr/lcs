# Changelog

## Next

- Added terminal colors to `lcs status` and `lcs resource list`: green for healthy states, red for unavailable or failed states, and yellow for transitional or administratively stopped states. Use `--no-colors` to disable colors. Redirected output, JSON, and `nrpe` remain plain.

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
