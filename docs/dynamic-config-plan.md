# Dynamic distributed resource configuration migration plan

Status: implementation in progress. Checked items have been implemented; unchecked
items remain future work. The current stage prepares local JSON snapshots while
INI remains authoritative. Distributed activation and runtime definition commands
are not implemented yet. See [resource-json.md](resource-json.md) for current behavior.

The goal is to create, modify, and delete resources and groups through `lcs`
while the cluster runs. The daemon will distribute and persist their JSON
definitions on every voting member, including quorum-only nodes. Existing
installations must remain usable throughout the upgrade and migration.

## Proposed boundaries and storage

The human-maintained `/etc/lcs/lcs.conf` continues to define cluster identity,
members, roles, networking, security, timing, and other daemon settings. Dynamic
management initially covers resource and group definitions only. Membership
changes and persistent lease/epoch storage are outside this refactor.

Proposed default paths:

- `/etc/lcs/resources.json`: daemon-maintained committed dynamic configuration,
  next to the static configuration. Operators inspect/export it but use CLI
  transactions to change it.
- `/var/lib/lcs/`: durable configuration transaction and recovery metadata.
  Its contents must survive daemon restarts, machine reboots, and package upgrades.
- `/run/lcs/`: existing temporary sockets and other runtime files.

The committed document is a complete snapshot containing a schema version,
cluster identity, committed revision, groups, and resources keyed by name.
Agreement metadata must identify the exact content and history, rather than
trusting a revision counter alone. The precise transaction file layout remains
to be designed. Resource ownership, leases, epochs, and temporary administrative
stop flags retain their current runtime semantics.

## Upgrade experience

Administrators upgrade the packages and restart nodes one by one. Each upgraded
daemon automatically prepares JSON from the effective INI resource/group
definitions. During the mixed-version period, INI startup and coordinated reload
remain authoritative, and dynamic mutation commands remain unavailable. Prepared
JSON is refreshed from successfully applied INI changes during this period.

Once every configured member, including quorum-only nodes, has confirmed support
and readiness with compatible effective definitions, the cluster automatically
commits a durable activation of dynamic configuration. No migration command or
manual removal of INI sections is required. Activation preserves running resources
and establishes JSON as the resource authority; retained INI resource/group
sections are bootstrap history, not an alternative source of definitions.

An offline member that has not confirmed readiness delays initial activation.
Once activation is committed, later disconnections do not revert to legacy mode
or require every member to remain online for subsequent majority-based edits.
Activation and configuration synchronization must complete locally before a node
can serve dynamic mutations or participate under the new configuration rules.

## 1. Specify the contract before implementation

- [x] Define the JSON schema for every supported resource/group option, required
  fields, defaults, limits, unknown fields, duplicate keys, and schema evolution.
- [x] Choose a maintained JSON parser and document its build, licensing, and
  packaging impact; preserve strict input validation and bounded allocation.
- [x] Define canonical snapshot encoding independent of JSON object order and
  dependency order, including cluster identity and schema compatibility checks.
- [x] Define the content digest and agreement-history identity for distributed
  transactions; do not treat the legacy voting fingerprint as the full JSON digest.
- [x] Define stable resource identity across edits, sorted wire-ID remapping,
  deletion/recreation, and stale resource messages after configuration changes.
- [x] Define storage paths, ownership, permissions, and isolated paths
  for multiple local daemons and integration tests.
- [x] Specify CLI success, rejection, pending, and outcome-unknown responses;
  a client timeout must not imply an aborted transaction.

Application, identity, recovery and client-outcome rules are specified in
[the application contract](dynamic-config-application.md). Checked design items
do not imply that their daemon integration has been implemented.

## 2. Design durable distributed agreement and recovery

Existing reload coordinates in-memory candidates loaded independently from INI
files. It is a foundation for validation and resource transitions, not a complete
durable distributed configuration protocol.

- [x] Specify transaction identifiers, expected base revision/digest, coordinator
  selection, durable promises/terms, and concurrent-proposal serialization.
- [x] Specify which participants must agree and persist each stage. Require a
  voting majority and suitable full-member participation; define treatment of
  online nonparticipants and incompatible members explicitly.
- [x] Prove that intersecting majorities cannot commit different successors to
  the same configuration, including after coordinator failure and restart.
- [x] Define proposal, agreement, resource drain, commit, application, and client
  acknowledgement ordering. State exactly which records must reach durable
  storage before each response or resource operation.
- [x] Define safe handling of active changed/deleted resources, dependents,
  unreachable former owners, lease expiry, and `stop_failed` before activation
  under a new configuration. Preserve current reload safety guarantees.
- [x] Specify crash recovery at every boundary, including configuration committed
  durably but not yet applied, and drains completed before a failed transaction.
- [ ] Persist last-applied definitions and pending backend cleanup information
  before resource transitions. Retain enough information to stop deleted/replaced
  backends after a crash, even when catch-up crosses multiple revisions.
- [x] Add persistent resource incarnation identity so deletion/recreation cannot
  reuse an old name's lease/grant state on returning nodes. Schema-2 snapshots,
  candidate allocation and agreement validation are implemented; daemon resource
  message admission and runtime remapping still require integration.
- [x] Specify recovery when JSON and journal disagree or one is missing/corrupt.
  Fail safely with actionable diagnostics; never silently reset to empty state.
- [x] Implement local atomic record replacement, file and parent-directory
  syncing, exclusive locking, and bounded regular-file reads.
- [x] Implement and test persistent acceptor records (promises, accepted values,
  committed predecessor/value, acknowledgement mask, node/membership binding,
  and checksum) plus proposer selection and majority tracking. These cores are
  not yet wired into daemon activation. See [agreement design](dynamic-config-agreement.md).
- [x] Implement nonblocking storage workers, bounded result handoff, monotonic
  state import, and cancellation that waits for child exit before disk recovery.
- [ ] Implement multi-revision catch-up, durable request-status history, and
  safe compaction/checkpointing for the complete distributed transaction manager.
- [x] Define full-cluster restart, stale-majority prevention, disk loss, backup
  restoration, and explicit disaster recovery. A lone high revision or a former
  minority must not establish authority by itself.
- [x] Require a stale/recovering node to synchronize configuration and reconcile
  old local resources before voting or hosting resources. Quorum-only members
  persist the same committed definitions and relevant agreement metadata.

## 3. Separate configuration loading from resource application

- [x] Extract a shared candidate model and validator used by INI reload, JSON
  import, distributed proposals, and CLI edits.
- [x] Automatically prepare JSON on first upgraded startup while keeping INI
  startup/reload authoritative until coordinated automatic activation.
- [ ] Use one resource/group authority at a time: INI before activation, JSON
  after activation. Do not introduce a permanent mixed static/dynamic namespace.
- [ ] Route existing coordinated reload and dynamic updates through compatible
  serialization rules so they cannot race or overwrite each other's changes.
- [ ] Preserve unchanged resources' ownership and runtime state; reuse safe
  resource transition and dependency-ordering machinery for affected resources.
- [ ] Define how read-only status/export reports configuration origin, committed
  revision, and pending operations, without treating file edits as commits.

## 4. Add protocol support and synchronization

- [ ] Allocate a new wire protocol version and feature gates following
  [version.md](version.md); preserve the released HELLO prefix and wire-5 peering
  during the supported rolling-upgrade phase.
- [ ] Define bounded snapshot transfer, transaction messages, revision/digest
  exchange, reconnect synchronization, and retry/idempotency behavior.
- [x] Implement transport-independent bounded BEGIN/CHUNK snapshot codecs,
  exact metadata validation, fixed deadlines, duplicate handling, membership
  checks, and complete-value validation. Peer feature registration is still pending.
- [ ] Gate automatic activation on explicit capability and readiness confirmation
  from every configured member, including quorum-only nodes, with matching
  effective configuration. Unknown/offline members must not count as upgraded.
- [ ] Design a durable activation barrier serialized with INI reload and dynamic
  transactions; version advertisements alone must not switch local authority.
- [ ] Persist activation across restarts and catch up lagging nodes. After
  activation, use normal quorum requirements for edits rather than requiring
  all members to stay online; never automatically revert to legacy mode.
- [ ] After activation, prevent older binaries or stale static configurations
  from rejoining as voters/owners under an incompatible resource definition.
- [ ] Apply existing peer security checks to configuration transfer and mutation;
  restrict local mutation commands through the Unix socket access policy.

## 5. Add runtime management commands

Command names below are proposals to finalize before implementation. Existing
`resource start/stop/move` remain runtime controls with their current semantics.

- [ ] Model new resource/group definition commands on `pcs` syntax and conventions
  wherever practical, so administrators familiar with `pcs` can use familiar
  command structure, resource identifiers, option names, and `key=value`
  arguments for creation, modification, and deletion.
- [ ] Before finalizing the CLI, compare the proposed commands with supported
  `pcs` commands and document equivalent LCS examples. Prefer familiar forms such
  as `lcs resource create`, `lcs resource update`, and `lcs resource delete` where
  their semantics match; preserve existing LCS commands and safety requirements.
- [ ] Document unavoidable syntax or behavior differences explicitly, including
  LCS resource types, supported options, group semantics, and deletion rules.
  Do not imply support for Pacemaker resource agents or unsupported `pcs` features.
- [ ] Add resource `create`, `update`, and `delete`, plus corresponding group
  operations, routed through one whole-snapshot transaction path.
- [x] Implement and unit-test the internal candidate editor for pcs-style resource
  create/update/delete/remove, group membership changes, and group policy updates.
  Keep it disconnected from live CLI mutation endpoints until agreement is ready.
- [ ] Define field replacement, explicit field removal, immutable fields, and
  rejection of invalid group/dependency deletions; avoid implicit cascading deletes.
- [ ] Add configuration export, validation, and atomic bulk import commands.
- [ ] Support expected-revision checks and idempotent request IDs; expose a
  transaction status command so interrupted clients can determine the outcome.
- [ ] Return actionable errors for no quorum, unsupported members, revision
  conflicts, invalid definitions, persistence failures, and failed resource stops.
- [ ] Provide structured `--json` results and consistent CLI exit codes.

## 6. Support automatic upgrade and migration from existing releases

- [ ] Test rolling binary upgrades from the currently released wire-5 version
  with existing INI resources, unchanged placement, and no dynamic files present.
- [ ] Implement deterministic automatic bootstrap from effective INI definitions,
  preserving resource identity, ownership, lease safety, and running resources.
  Use compatible voting fingerprints during the rolling-upgrade phase.
- [x] Refresh prepared JSON after each successful legacy INI reload and startup.
  Perform runtime filesystem work in a background worker, coalescing refreshes
  without blocking lease renewal.
- [ ] Serialize final conversion/activation against pending reloads and workers
  so an earlier prepared snapshot cannot replace newer agreed resource definitions.
- [ ] Automatically activate dynamic authority after every member confirms
  capability, matching configuration, and durable readiness. No administrator
  migration command or configuration cleanup may be required.
- [ ] Persist resumable initialization and activation metadata. Distinguish a
  never-initialized node from lost/corrupt state; missing JSON after initialization
  must trigger recovery rather than reimporting stale INI resources.
- [ ] Leave human-maintained INI files untouched. After activation, ignore their
  resource/group definitions as configuration inputs, report their inactive
  status clearly, and direct attempted INI resource edits to runtime commands.
- [ ] Define parsing/reload behavior for retained INI sections, static daemon
  settings, new installations, and newly provisioned stale nodes joining an
  already activated cluster; synchronize committed state before participation.
- [ ] Document rollback before activation and a deliberate export/conversion path
  after activation. Downgrading binaries alone must not silently discard state.
- [ ] Verify migration interruption, failed writes, node reboot, and offline-node
  return; retrying migration must not create resources twice or revert a commit.

## 7. Update installation, packaging, and service integration

- [x] Update `Makefile` installation and `debian/rules` / `debian/lcs.postinst`
  so fresh installs and upgrades create `/var/lib/lcs/` with explicit ownership
  and permissions before any daemon restart.
- [x] Update `packaging/package-rpm.sh` to create and own the state directory in
  the generated package and handle upgrades without replacing persisted contents.
- [x] Add `StateDirectory=lcs` and an explicit mode to
  `packaging/lcsd.service` as appropriate; retain the runtime directory and
  support direct daemon execution without relying exclusively on systemd.
- [x] Ensure `/etc/lcs/` permits the intended daemon writer to atomically replace
  the JSON snapshot; document permissions for custom paths.
- [x] Never ship a live empty JSON snapshot that overwrites existing state.
  Keep examples separate from daemon-generated configuration and journal files.
- [ ] Define uninstall/purge and reinstall behavior explicitly. Preserve live
  state by default; any deletion must be deliberate and documented.
- [ ] Test Debian and RPM fresh install, upgrade, reinstall, removal, and
  `DESTDIR` staging; verify directory creation occurs before service startup.

## 8. Verify safety and publish the migration procedure

- [x] Unit-test local schema validation, canonicalization, candidate construction,
  bootstrap recovery, atomic-write failure boundaries, and background refresh.
- [ ] Unit-test revision conflicts, distributed journal recovery, and
  malformed/oversized wire transfers.
- [x] Test the agreement core against competing coordinators, accepted-majority
  recovery after restart, duplicate/conflicting replies, corrupt/foreign journals,
  write/sync failures, old worker replays, cancellation, large worker results,
  and malformed/incomplete/expired snapshot transfers.
- [ ] Integration-test create/update/delete/import with VIPs, services, groups,
  dependencies, quorum-only voters, and existing runtime controls.
- [ ] Test simultaneous edits, partitions, quorum loss, coordinator changes,
  delayed/duplicate messages, stale reconnects, and configuration ID remapping.
- [ ] Inject crashes at persistence and resource-transition boundaries; verify
  acknowledged updates survive restarts and conflicting updates cannot commit.
- [ ] Test full-cluster restart, stale snapshots, corrupt files, full disks,
  permission failures, and interruption of checkpointing and migration.
- [ ] Verify legacy INI behavior during mixed-version rolling upgrades, automatic
  activation only after every configured node is ready, reload during upgrade,
  partial activation recovery, and continued dynamic operation with a node offline.
- [ ] Update `docs/config.md`, `docs/manual.md`, packaged examples, man pages,
  release notes, and version locations from `docs/version.md`. Keep advanced
  operational detail out of README.
- [ ] Publish operator instructions for upgrade, migration, backup/restore,
  recovery, rollback, and package lifecycle behavior before releasing the feature.

Implementation should follow the numbered phases. Do not expose mutating commands
as supported functionality until durable agreement, crash recovery, compatibility,
and migration tests are complete.
