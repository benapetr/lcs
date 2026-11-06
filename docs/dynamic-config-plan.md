# Dynamic distributed resource configuration migration plan

Status: implementation in progress. Checked items have been implemented; unchecked
items remain future work. Normal upgrades prepare local JSON snapshots while
INI remains authoritative; already-activated durable catalogs can resume through
fenced daemon recovery. Automatic distributed activation and runtime definition
commands are not implemented yet. See [resource-json.md](resource-json.md) for current behavior.

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
- [x] Persist last-applied definitions and pending backend cleanup information
  before resource transitions. Retain enough information to stop deleted/replaced
  backends after a crash, even when catch-up crosses multiple revisions. The
  tested local application-catalog core retains complete old/pending snapshots
  and computes incarnation/dependency cleanup plans; daemon workers and backend
  application are not connected yet.
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
  Agreement and application begin/finish now share one writer; catalog imports
  cannot erase pending cleanup obligations or replay older applied state.
- [x] Implement durable committed checkpoint catch-up across multiple revisions,
  retaining higher promises and applied/pending cleanup snapshots. Keep normal
  worker imports strict and validate incarnation continuity on checkpoint imports.
- [ ] Implement durable request-status history, backup/recovery policy and safe
  history retention/compaction for the complete distributed transaction manager.
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
- [x] Recover and validate existing agreement/application records before INI
  bootstrap can write snapshots or the daemon can open listeners. The current
  daemon resumes supported activated catalogs through fenced recovery and refuses
  unsupported initialization/history states; it never silently falls back to INI.
- [x] Persist the exact revision-zero baseline before creating agreement or
  application records. Resume interrupted local initialization from that frozen
  value, reject missing records after preparation, and block INI snapshot refresh
  once the intent exists. This local prerequisite does not authorize activation;
  coordinated initialization remains unfinished.
- [x] Run local initialization through the shared nonblocking storage worker,
  excluding INI refresh and withholding prepared handles until successful child
  exit. Preserve recovery requirements on cancellation, uncertain writes and
  conflicting history. The distributed manager does not invoke it yet.
- [ ] Use one resource/group authority at a time: INI before activation, JSON
  after activation. Do not introduce a permanent mixed static/dynamic namespace.
- [ ] Route existing coordinated reload and dynamic updates through compatible
  serialization rules so they cannot race or overwrite each other's changes.
- [x] Add an explicit internal application sequencer through the shared storage
  coordinator: persist pending cleanup before callbacks, enforce quiesce/drain/
  old-owner/install ordering, persist the applied marker before admission, and
  hold voting until admission succeeds. Resume exact pending targets across
  newer checkpoints, skip backend work for quorum-only nodes, and retain holds
  on failed cleanup or uncertain persistence. Live adapters remain unfinished.
- [x] Bind dynamic resource requests/replies, state sync and voting heartbeats to
  complete applied generation digests, membership and daemon instances before
  resource dispatch or RPC completion. Reject in-place session generation changes;
  keep configuration catch-up available for mismatches and reject wire-5 dynamic
  sessions. Explicit install/admit APIs cancel indexed peer operations before
  remapping and require durable matching applied state before generation traffic.
  Live backend/remap/startup adapters are not connected yet.
- [x] Implement explicit local backend callbacks with definition fences, existing
  stop machinery/old hooks, dependency/post-stop ordering, failed-cleanup blocking,
  incarnation-aware sorted remapping and preserved unchanged ownership/promises.
  Pause indexed traffic/work without extending existing leases through missing
  quorum/admission; reset replacement incarnations and wait for generation recovery.
- [x] Publish the latest learned schema-2 snapshot to `resources.json` through the
  shared storage worker, with revision-bound completion, retryable projection file
  errors, and a concrete publisher for the explicit local adapter.
- [x] Add internal old-owner proof primitives: exact catalog transition/drain
  scopes, pinned local stop receipts, strict message codecs, and instance-bound
  collection from every full member. No majority, disconnect or timer implies
  stop; failed cleanup, contradictory replies and session changes invalidate proof.
- [x] Connect the old-owner proof collector to admitted wire-6 peer transport
  and explicit local drain callbacks for one pinned transition, with bounded
  responses/retries, instance teardown invalidation, cleanup-failure revocation
  and a final pre-install barrier recheck. Legacy queries remain nonmutating.
- [x] Retire stop receipts only after instance-bound confirmation of exact durable
  application from every full member. Retain the preceding completion scope for
  slow consumers; replace it only after every full member applies the next scope.
  Support consecutive transitions without unbounded history or timer eviction.
- [x] Add explicit startup cleanup from the recovered last-applied snapshot,
  including old hooks, dependency ordering, bounded asynchronous backend work,
  retained cancellation/reaping and snapshot-bound completion. Recreate a pending
  stop scope only after verified cleanup under the new daemon instance. Bind
  backend/hook process groups to daemon lifetime to prevent late orphan work.
- [x] Persist the two preceding complete applied snapshots atomically with
  FINISH, validate/import their exact lineage, and clean retained historical
  backends on restart. Restore the latest completed owner scope and preceding
  completion record only after snapshot-bound physical cleanup, then collect
  fresh all-full-member application confirmation before a pending scope proceeds.
- [x] After verified startup cleanup, answer differing exact old-generation
  scopes when both values are retained and the target is current applied or
  pending. Recompute drain masks, confirm APPLIED only for the exact current
  applied target, and revalidate buffered replies before sending. Keep recovery
  fences held; this explicit provider does not perform admission or change receipts.
- [x] Add an explicit recovery handoff across durable catalog progress: require
  fresh single-use cleanup, retain the old provider until replacement, preserve
  daemon attempt serials/instances, and reconstruct only still-answerable exact
  scopes. Exercise two full members with differing pending/applied targets through
  pending completion and fresh all-member retirement, without skipping the saved
  pending revision or admitting leases.
- [x] Add an explicitly started nonblocking recovery controller that sequences
  cleanup, receipt retirement, shared-worker BEGIN/FINISH, exact pending owner
  collection, mandatory fenced table installation, and fresh cleanup handoffs.
  Resume differing pending/applied targets and new checkpoints while retaining the
  application/voting hold through caught-up readiness. Exercise repairs, session
  loss, uncertain storage, cancellation, quorum-only operation and missing evidence.
- [ ] Recover missing predecessor history from older catalogs in the distributed
  daemon manager; keep unsupported scopes blocked.
  Catalog revision numbers alone do not reconstruct backend cleanup proof.
- [x] Add the concrete recovery table adapter: prepare last-applied schema-2
  definitions before listeners under pristine-state checks, establish an
  unadmitted generation and global fences, and install only the controller's exact
  verified cleanup/owner scope. Remap matching incarnation promises/policies,
  normalize observed ownership after physical cleanup, reset replacements, and
  preserve fences through generation-session cancellation and proof retries.
- [x] Require latest-learned resources.json publication through the shared writer
  before recovery table installation, including already-current/quorum-only tables
  and retries of older pending targets. Retain fences/catalogs on known projection
  errors, recheck newer checkpoints after delayed completions, and retain borrowed
  storage until cancelled projection workers are reaped.
- [x] Add explicit two-stage admission after caught-up recovery: enable only
  matching-generation restart sync while grants/configuration votes remain fenced,
  require the ordinary restart floor, fresh admitted peer sync and quorum, then
  detach owned cleanup providers and transfer the coordinator/network without
  resetting serials or promises. Re-fence on new learning, authority loss or stop;
  preserve the latest completion tombstone for slow peers after transfer.
- [x] Connect existing activated-catalog recovery, the startup table adapter,
  projection publisher and distributed old-owner provider to daemon startup;
  drive the explicit admission handoff from the scheduler. Fence CLI mutations
  and normal placement while recovering; retain cleanup workers through shutdown.
  Exercise differing pending revisions, historical hooks, failed-cleanup repair,
  quorum-only participation, edited INI exclusion and restart with a missing
  projection in real processes.
- [ ] Connect the local application adapter/publisher and distributed old-owner
  provider to normal running-daemon definition updates. Until then, new learned
  definitions after admission must fence and stop the daemon for restart recovery.
  Restart cleanup and any lease-expiry/external fencing path require explicit
  validated evidence; no default proof is permitted.
- [ ] Define how read-only status/export reports configuration origin, committed
  revision, and pending operations, without treating file edits as commits.
  Legacy INI origin and prepared-member diagnostics are available through
  `lcs config status`; committed/applied revision and transaction outcomes still
  require the durable manager.

## 4. Add protocol support and synchronization

- [x] Allocate a new wire protocol version and feature gates following
  [version.md](version.md); preserve the released HELLO prefix and wire-5 peering
  during the supported rolling-upgrade phase.
- [x] Exchange advisory bootstrap preparation and full membership/value digests
  over negotiated wire-6 sessions. Expose local and per-member preparation through
  read-only `lcs config status`, clearing observations at session teardown and
  marking preparation unavailable during reload/persistence. This is not the
  durable activation-readiness barrier; dynamic mutations remain disabled.
- [ ] Define bounded snapshot transfer, transaction messages, revision/digest
  exchange, reconnect synchronization, and retry/idempotency behavior.
- [x] Implement transport-independent bounded BEGIN/CHUNK snapshot codecs,
  exact metadata validation, fixed deadlines, duplicate handling, membership
  checks, and complete-value validation. Wire-6 acceptor registration is implemented
  below; network proposer and checkpoint synchronization are implemented below.
- [x] Implement the local durable all-member readiness barrier, including
  quorum-only members, exact baseline/membership binding, a checksummed record,
  serialized nonblocking persistence, and application-aware configuration-vote
  eligibility. Recover proof before INI refresh and reject orphan or premature
  readiness. Automatic initialization and peer configuration-transaction admission
  remain unfinished.
- [x] Add distinct wire-6 readiness query/reply messages with strict codecs,
  established-peer and instance binding, monotonic attempt correlation, a fixed
  deadline, and collection from every configured member. INI-only nodes answer
  not-ready without creating files. Starting collection is still an internal
  coordinator operation; startup does not trigger initialization or activation.
- [x] Reject INI reload requests and peer reload transitions after local definitions
  are frozen, so explicit durable readiness cannot race a legacy resource reload.
- [x] Hand a completed explicit peer readiness round to the shared barrier writer
  from the scheduler. Keep collection, persistence, durable eligibility, failure
  and cancellation distinct; discard results for replaced attempts and wait for
  reaping before releasing runtime/storage handles during shutdown.
- [x] Gate explicit local coordinator PREPARE/ACCEPT writes on durable readiness
  and caught-up application, using that same writer. Require schema-2 successors
  and unchanged definitions for activation; distinguish durable vote rejection
  from uncertain persistence. Network acceptor framing/admission is implemented below; automatic proposal
  coordination is implemented below.
- [x] Register wire-6 acceptor control and bounded snapshot messages for PREPARE,
  ACCEPT and LEARN, with source/target instance and proposer-index checks. Send
  success only after durable worker completion; return unknown on uncertain I/O,
  isolate session buffers, and retain in-flight writes after disconnect.
- [x] Learn an exact immediate schema-2 decision only with a configured majority
  including a full member. Keep learned/applied state separate, block new votes
  while application lags, and acknowledge exact decision retries idempotently.
  A three-node transport harness exercises promises, accepted-value recovery,
  acceptance quorum and decision learning.
- [x] Add the internal network proposer: persist local PREPARE before sending,
  correlate admitted peer instances and exact requests, recover complete accepted
  values, gather distinct promises/ACKs, and persist the chosen decision locally.
  Use one fixed deadline and shared storage writer; report unknown on interruption,
  and require newer IDs/ballots for retries. Notify acknowledged acceptors, leaving
  nonparticipating/reconnected members to checkpoint synchronization.
  This explicit internal entry point does not automatically activate or apply resources.
- [x] Pull committed checkpoint snapshots over admitted wire-6 sessions, binding
  requests to instances, static membership and frozen baseline. Validate original
  majority decisions relayed by any configured member, serialize persistence,
  and poll peers in rotation only after a durable barrier. Cover revision gaps,
  delete/recreate cleanup, quorum-only receivers, deadlines and disconnected writes.
  Learning remains separate from application and distributed startup admission.
- [ ] Gate automatic activation on explicit capability and readiness confirmation
  from every configured member, including quorum-only nodes, with matching
  effective configuration. Unknown/offline members must not count as upgraded.
- [ ] Connect the durable activation barrier to live INI reload and dynamic
  transaction serialization; version advertisements alone must not switch local
  authority.
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
