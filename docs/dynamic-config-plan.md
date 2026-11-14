# Dynamic distributed resource configuration migration plan

Status: functional integration is implemented; the next phase is runtime
availability under storage failure, before history-retention work and the wider
fault matrix. The functional checklist was reconciled through implementation commit
`6f4db5c` in `304cb2f`; the storage-isolation requirement below supersedes the earlier
ordering of remaining priorities. Checked items describe implemented work, existing
test coverage, or an explicitly agreed contract; unchecked items describe remaining
implementation, policy or validation work.
They do not imply that the feature is ready for release.

Automatic INI-to-JSON activation, runtime resource/group commands, durable majority
agreement, live application and supported restart/catch-up recovery are implemented.
Mixed wire-5/6 clusters retain INI authority until every configured member supports
migration and has matching definitions and healthy startup recovery. Activation
then temporarily drains old resources before admitting the dynamic generation.
See [resource-json.md](resource-json.md) for current behavior.

The immediate priority is proving and enforcing that storage stalls cannot stop
ordinary operation of the already-loaded cluster. Background persistence workers
are implemented, but their existence alone does not establish this guarantee.
After that phase, address history-capacity/retention, retained-INI/static-setting
semantics, operator backup/recovery/rollback procedures and package lifecycle
policy, then expand the partition/crash/storage-failure matrices. Lost-history
transfer remains an implementation gap; unsupported recovery must fail closed.

The goal is to create, modify, and delete resources and groups through `lcs`
while the cluster runs. The daemon distributes and persists their JSON
definitions on every voting member, including quorum-only nodes. Existing
installations must remain usable throughout the upgrade and migration.

## Boundaries and storage

The human-maintained `/etc/lcs/lcs.conf` continues to define cluster identity,
members, roles, networking, security, timing, and other daemon settings. Dynamic
management initially covers resource and group definitions only. Membership
changes and persistent lease/epoch storage are outside this refactor.

Implemented default paths:

- `/etc/lcs/resources.json`: daemon-maintained projection of the committed dynamic
  configuration, next to the static configuration. Operators inspect/export it but
  use CLI transactions to change it; this file alone is not a complete backup of
  agreement and application history.
- `/var/lib/lcs/`: durable configuration transaction and recovery metadata.
  Its contents must survive daemon restarts, machine reboots, and package upgrades.
- `/run/lcs/`: existing temporary sockets and other runtime files.

The committed document is a complete snapshot containing a schema version,
cluster identity, committed revision, groups, and resources keyed by name.
Agreement metadata must identify the exact content and history, rather than
trusting a revision counter alone. The implemented activation, agreement,
application, request and readiness records are described in
[the agreement contract](dynamic-config-agreement.md) and
[storage documentation](resource-json.md#storage-and-upgrades).
Resource ownership, leases, epochs, and temporary administrative stop flags retain
their runtime semantics; they are not a persistent lease-state database.

## Upgrade experience

Administrators upgrade the packages and restart nodes one by one. Each upgraded
daemon automatically prepares JSON from the effective INI resource/group
definitions. During the mixed-version period, INI startup and coordinated reload
remain authoritative, and dynamic mutation commands remain unavailable. Prepared
JSON is refreshed from successfully applied INI changes during this period.

Once every configured member, including quorum-only nodes, has confirmed support
and readiness with compatible effective definitions, the cluster automatically
commits a durable activation of dynamic configuration. No migration command or
manual removal of INI sections is required. Activation preserves definitions and
lease safety, but temporarily stops old backends and reacquires ownership after
JSON admission. The mixed-version phase keeps ordinary INI placement and lease
rules; conversion itself does not promise uninterrupted ownership or an unchanged
epoch. JSON becomes the resource
authority. Retained INI resource/group sections no longer supply running resource
definitions, although the current startup path still parses and validates INI
before recovering JSON. Clarifying that parsing contract and improving inactive-
section diagnostics remain open below.

An offline member that has not confirmed readiness delays initial activation.
Once activation is committed, later disconnections do not revert to legacy mode
or require every member to remain online for subsequent majority-based edits.
Activation and configuration synchronization must complete locally before a node
can serve dynamic mutations or participate under the new configuration rules.

## Next phase: runtime availability under storage failure

This phase takes precedence over the remaining numbered implementation items.
The cluster may depend on storage to load/recover configuration at startup and to
persist changes to resource definitions or static cluster configuration. Once a
valid configuration is loaded and admitted, network-storage I/O may stall for tens
of seconds without stopping normal operation under that configuration.

| Operation | Storage dependency permitted? |
| --- | --- |
| Heartbeat send/receive and network membership tracking | No |
| Operational quorum tracking and lease votes/renewals | No |
| Ownership tracking, ordinary failover and moves of existing VIPs | No |
| Resource/group definition changes and their durable agreement | Yes; changes may remain pending, fail or be refused while storage is unavailable |
| Initial configuration load and recovery after restart | Yes; recovery still requires valid configuration and safety evidence |

Configuration agreement votes are separate from operational lease votes. Pausing
configuration persistence must not withdraw operational quorum or invalidate the
healthy applied generation merely because its writer is slow or unavailable.
Runtime ownership, leases, epochs, stop receipts and heartbeat/communication state
remain in RAM. Existing configuration journals concern definition transactions;
this phase does not add persistent runtime lease or move records, and it does not
remove necessary configuration agreement/cleanup evidence without a safe design.

A resource being deleted/replaced may need to remain fenced while its definition
transition is unresolved. That does not authorize globally fencing unrelated
resources, stopping their renewals or preventing ordinary VIP moves. Actual
conflicting ownership, unverified backend cleanup or invalid configuration proof
still requires the appropriate safety fences. A storage delay/error alone is not
proof of a network partition or loss of operational quorum.

### Ordered implementation and acceptance steps

- [x] Agree on the architectural boundary above: storage may block persistent
  configuration changes and startup recovery, but cannot be a dependency for
  ordinary operation of the healthy, already-loaded cluster.
- [ ] Audit the active event-loop and runtime paths before changing behavior.
  Trace heartbeat/membership, operational quorum, leases, moves, configuration
  writer failure/timeout, projection/outcome publication, logging and shutdown.
  Classify storage operations and every transition to global/resource-specific
  fences; distinguish actual proof loss from persistence unavailability.
  Record which checks protect definitions being changed versus the already-valid
  applied generation. Do not label existing behavior compliant without evidence.
- [ ] Build test-only storage fault injection with explicit reached/release
  synchronization. Pause selected read/write/fsync/rename/directory-sync workers
  for 30 and 60 seconds, and inject errors separately. Cover existing storage
  children and writer queues without sleeping in the production event loop or
  making the whole test host unavailable. Artificial delays are test machinery,
  not a normal daemon feature or deployment setting.
- [ ] Establish baseline regressions under an unchanged admitted configuration:
  storage unavailable, sustained heartbeats/lease renewals, CLI responsiveness,
  an explicit existing-VIP move, and owner failure followed by takeover using
  the surviving network quorum. Test loss/stall of storage on one node and on
  all running nodes. Keep CPU, memory, network and built-in VIP backend available
  so these tests isolate configuration-storage dependence.
- [ ] Add in-flight configuration-change regressions with at least one unrelated
  running VIP. Pause/fail reservation, agreement, catalog, projection and final
  request-outcome writes at distinct boundaries. The new definition operation
  may be pending/rejected/unknown as appropriate; unrelated VIP renewal, move
  and failover must continue. Preserve the exact safety obligations for changed
  backends and never report unpersisted changes as durably successful.
- [ ] Separate configuration-writer failure/busy state from operational admission,
  quorum and lease/move eligibility for the healthy applied generation. Replace
  unnecessary global fences with the required transition-specific holds, and
  keep runtime scheduling active while persistence is pending or unavailable.
  Audit request-status/outcome bookkeeping too: failure to persist a client
  receipt alone must not revoke an otherwise valid running configuration.
- [ ] Isolate incidental blocking storage paths, particularly synchronous stderr
  and syslog output. Define bounded, nonblocking/best-effort logging behavior under
  backpressure; test a blocked sink independently of configuration writers.
  Configuration durability must not depend on a log line being written.
- [ ] Handle long-lived/stuck workers without blocking the event loop on reaping,
  spawning competing writers, thawing uncertain configuration or replaying late
  results against a newer attempt. Verify release/late completion after the stall,
  continued operation during the stall, and safe retry/recovery afterward.
- [ ] Measure heartbeat/renewal gaps and record operational quorum/owner timelines
  during the injected stalls; a final healthy status alone is insufficient.
  Assert no lease expiry, false peer loss or resource loss caused by storage
  waits. During deliberate owner failure, allow only the expected failure-detection
  and lease-expiry interval before takeover; check for duplicate VIP ownership.
- [ ] Document the proven availability contract and any remaining dependencies,
  update the implementation evidence, and complete the targeted regressions before
  returning to history retention and the broader release-testing matrix.

### Current audit leads, not completed fixes

- [scheduler.c](../src/scheduler.c) continues ordinary runtime work during many
  background jobs, but skips it while the configuration manager is recovering or
  failed; inspect whether persistence-only failures can take those paths.
- [config_manager.c](../src/config_manager.c) has a global failure path that clears
  operational voting/admission, and application/outcome work depends on the shared
  writer. Distinguish configuration safety failures from writer unavailability.
- [cluster.c](../src/cluster.c) gates operational quorum/voting on configuration
  remapping/admission flags; audit the provenance and lifetime of those flags.
- [log.c](../src/log.c) calls stderr stdio and syslog synchronously. The current
  child-based configuration writer does not isolate these logging calls.

These are audit entry points, not a claim that every storage error currently
stops VIPs. Existing asynchronous I/O and crash-boundary tests do not establish
30–60-second storage-stall survival for heartbeats, operational quorum and moves.

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
  and computes incarnation/dependency cleanup plans. Daemon workers, live backend
  application and startup recovery consume these catalogs.
- [x] Add persistent resource incarnation identity so deletion/recreation cannot
  reuse an old name's lease/grant state on returning nodes. Schema-2 snapshots,
  candidate allocation, agreement validation, daemon resource-message admission
  and incarnation-aware runtime remapping are implemented.
- [x] Specify recovery when JSON and journal disagree or one is missing/corrupt.
  Fail safely with actionable diagnostics; never silently reset to empty state.
- [x] Implement local atomic record replacement, file and parent-directory
  syncing, exclusive locking, and bounded regular-file reads.
- [x] Implement and test persistent acceptor records (promises, accepted values,
  committed predecessor/value, acknowledgement mask, node/membership binding,
  and checksum) plus proposer selection and majority tracking. These cores drive
  daemon activation and runtime definition transactions. See [agreement design](dynamic-config-agreement.md).
- [x] Implement nonblocking storage workers, bounded result handoff, monotonic
  state import, and cancellation that waits for child exit before disk recovery.
  Agreement and application begin/finish now share one writer; catalog imports
  cannot erase pending cleanup obligations or replay older applied state.
- [x] Implement durable committed checkpoint catch-up across multiple revisions,
  retaining higher promises and applied/pending cleanup snapshots. Keep normal
  worker imports strict and validate incarnation continuity on checkpoint imports.
- [x] Integrate durable request reservations, learned-decision facts, originating
  client descriptors and local admitted/applied receipts with the transaction
  manager and public request-status/mutation results.
- [ ] After runtime/storage isolation, define and implement safe request-history
  retention/compaction or an explicit supported exhaustion policy. The current journal retains at most 128 identities
  and refuses new IDs without eviction; retries remain readable/idempotent.
  Capacity handling must preserve promises, accepted values, uncertain outcomes,
  cleanup obligations and immutable retry identities.
- [ ] Publish a consistent backup/restore and lost-state recovery procedure for
  agreement, application, activation, readiness and request history together.
- [x] Add an explicit local request-fact journal core: bind member/membership,
  immutable request ID/expected base/canonical value fingerprint, idempotent
  reservation and exact learned-decision observation. Persist atomically with
  checksums, retain all observed IDs at bounded capacity, and reject stale worker
  imports. Test restart, process exits, uncertain syncs, corruption and conflicts.
  A reservation/commit fact alone is not admission or a final client outcome;
  separate applied receipts below record historical local admission.
- [x] Add request reservation and learned-decision observation jobs to the same
  serialized storage worker as agreement/catalog work. Publish only after durable
  child completion; test writer exclusion, retries/conflicts, crashes, cancellation
  and recovery. History must already be explicitly opened; missing history is
  created only through guarded coordinated first initialization, never loss repair.
- [x] Recover existing request history at daemon startup against the frozen base;
  reject unsafe/corrupt/foreign/orphan records and locally contradictory learned or
  accepted identities before listeners open. Keep missing older history unavailable
  without silently creating an empty file or inferring checkpoint-gap outcomes.
- [x] Add explicit nonblocking request-history recovery/first-initialization jobs
  on the shared writer. Reopen and compare disk agreement before publishing;
  allow missing-file creation only with explicit first-initialization authorization
  at pristine revision zero. Refuse initialization after promises or learning,
  preserve corrupt state, and test uncertain writes, crashes and cancellation.
  This primitive does not infer first-initialization permission from missing files.
- [x] Route explicit request-history recovery, reservation and decision observation
  through the transaction coordinator's shared writer and shutdown lifecycle.
  Keep request completions separate from votes/catalog/projection completions;
  gate new IDs on voting eligibility and valid successors while allowing known-ID
  checks and learned observations during application lag without releasing fences.
  This storage entry point alone cannot authorize initialization or mutation;
  the public command path applies the separate admission checks.
- [x] Reserve the local proposer's original immutable request identity through
  the coordinator before PREPARE or proposal frames. Require healthy existing
  history, refuse known IDs even with newer transport IDs, and keep recovered
  accepted values separate from original intent. Test ordering, capacity, timeout,
  uncertain writes, shutdown/restart and absence of implicit history initialization.
- [x] Persist/check the acceptor's selected request identity before phase-two
  acceptance through the same coordinator writer. Require healthy existing
  history, reject immutable-ID conflicts and capacity exhaustion without accepting,
  and send positive ACK only after acceptance completes. Preserve exact protocol
  retries/recovery and test both writes' sync boundaries, failures and cancellation.
- [x] Observe successful LEARN/checkpoint decisions through the same writer when
  request history exists. Keep consensus completion separate from bookkeeping,
  preserve known decisions across observation failures, and fence configuration
  progress until observation completes or recovery is required. Record only the
  exact current proven outcome across checkpoint gaps; never initialize missing
  legacy history or report committed facts as application/client success.
- [x] Integrate coordinated first-initialization authorization and proposer/acceptor
  paths with public mutation endpoints. Persist local admission acknowledgements;
  unrecorded requests and checkpoint-gap outcomes remain unknown.
- [ ] Implement verified lost request-history recovery/checkpoint transfer without
  resetting journals or inferring missing outcomes. Keep missing mandatory history
  unavailable until that recovery is defined and implemented.
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
  once the intent exists. This local prerequisite alone does not authorize
  activation; the all-member freeze/readiness and majority decision do.
- [x] Run local initialization through the shared nonblocking storage worker,
  excluding INI refresh and withholding prepared handles until successful child
  exit. Preserve recovery requirements on cancellation, uncertain writes and
  conflicting history. The distributed manager invokes it only after explicit
  all-member initialization authorization.
- [x] Bind first request-history creation to a durable format-2 initialization
  intent: persist agreement, application and empty requests before PREPARED.
  Resume only while INITIALIZING; never recreate lost/corrupt PREPARED history
  or replace reservations made before PREPARE. Publish verified empty history
  only after the initialization child exits successfully, keep format-1 recovery
  compatible, and refuse to silently upgrade legacy intents. Cover all ten sync/
  crash boundaries, no parent-side I/O, cancellation and pre-listener recovery.
- [x] Use one resource/group authority at a time: INI before activation, JSON
  after activation. Do not introduce a permanent mixed static/dynamic namespace.
- [x] Route existing coordinated reload and dynamic updates through compatible
  serialization rules so they cannot race or overwrite each other's changes.
- [x] Add an explicit internal application sequencer through the shared storage
  coordinator: persist pending cleanup before callbacks, enforce quiesce/drain/
  old-owner/install ordering, persist the applied marker before admission, and
  hold voting until admission succeeds. Resume exact pending targets across
  newer checkpoints, skip backend work for quorum-only nodes, and retain holds
  on failed cleanup or uncertain persistence. Live adapters and the daemon manager
  are integrated below.
- [x] Bind dynamic resource requests/replies, state sync and voting heartbeats to
  complete applied generation digests, membership and daemon instances before
  resource dispatch or RPC completion. Reject in-place session generation changes;
  keep configuration catch-up available for mismatches and reject wire-5 dynamic
  sessions. Explicit install/admit APIs cancel indexed peer operations before
  remapping and require durable matching applied state before generation traffic.
  Live backend/remap/startup adapters use these APIs under the manager's fences.
- [x] Implement explicit local backend callbacks with definition fences, existing
  stop machinery/old hooks, dependency/post-stop ordering, failed-cleanup blocking,
  incarnation-aware sorted remapping and preserved unchanged ownership/promises.
  Pause indexed traffic/work without extending existing leases through missing
  quorum/admission; reset replacement incarnations and wait for generation recovery.
- [x] Publish the latest learned schema-2 snapshot to `resources.json` through the
  shared storage worker, with revision-bound completion, retryable projection file
  errors, and a concrete publisher for the explicit local adapter.
- [x] Recheck latest-learned projection publication at live table installation,
  including quorum-only nodes and exact retries of older pending targets.
  Require fresh established matching-generation synchronization and quorum for
  live admission; fence every grant until the sequencer transfers its hold.
  Re-fence synchronization on superseding learning, admission failure, authority
  loss and shutdown. Exercise repair, delayed checkpoints and stale peer state.
  Running-daemon manager integration is tracked separately below.
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
- [x] Connect the local application adapter/publisher and distributed old-owner
  provider to normal running-daemon learned-definition updates. Keep backend,
  hook, lease and peer progress running while placement/CLI mutations pause;
  preserve attempts and receipts across consecutive transitions. Retry repaired
  projections and transport rounds without clearing failed cleanup proof, retain
  children/storage through shutdown, and report progress through config status.
  Exercise checkpoint catch-up, live replacement/creation/policy edits/deletion
  and projection repair with both full-member processes continuously running.
  Separately exercise shutdown/restart during blocked application against a
  continuously running peer's retained owner receipt.
  Restart cleanup and any lease-expiry/external fencing path require explicit
  validated evidence; no default proof is permitted.
- [x] Report local JSON/INI authority and committed/applied/pending revision/digest
  through read-only `lcs config status`, including recovery/admission state and
  blocked errors. Keep prepared-peer metadata advisory and preserve legacy format;
  older clients must reject activated diagnostics rather than report INI authority.
- [x] Add read-only `lcs config export [committed|applied]` from effective INI or
  validated durable in-memory snapshots. Pin each response across learning/remaps,
  stream bounded chunks beyond one frame, allow recovery/application inspection,
  and reject incomplete or inconsistent replies without partial stdout.
- [x] Add read-only local request-fact diagnostics via `lcs config request-status
  ID` and `--json`. Expose reservation/commit facts with explicit unknown and
  unavailable/recovery-required history states, without disk I/O, inferred
  checkpoint-gap outcomes, automatic initialization or claims of client success.
- [x] Persist an exact local admitted/applied receipt only after real live or
  startup admission and its final handoff. Retain it monotonically across restart
  and later revisions, read older journals without inventing admission, and expose
  `applied` through local request status. Reject catalog rollback and do not infer
  completion from FINISH, current-catalog shortcuts or superseded attempts.
- [x] Persist the originating client descriptor atomically with original request
  intent before PREPARE. Bind exact ordered arguments, request ID, static
  membership and normalized expected base; retain the fingerprint across learning,
  restart and admission. Read older journals without inventing client evidence,
  reject descriptor replacement/attachment through worker imports, and exercise
  reservation timeout/cancellation/recovery. Public mutation/wait endpoints use
  this evidence for exact origin retries and structured results.
- [x] Expose origin-bound mutation results and local durable request facts through
  the public CLI, including applied, rejected, pending and unknown. An origin's
  historical local admission is not all-peer acknowledgement or current health.
- [ ] Define/implement recovery of missing request/outcome evidence across nodes
  and checkpoint gaps. Cross-origin queries/retries must retain unknown results
  where the original descriptor or durable outcome proof is unavailable.

## 4. Add protocol support and synchronization

- [x] Allocate a new wire protocol version and feature gates following
  [version.md](version.md); preserve the released HELLO prefix and wire-5 peering
  during the supported rolling-upgrade phase.
- [x] Exchange advisory bootstrap preparation and full membership/value digests
  over negotiated wire-6 sessions. Expose local and per-member preparation through
  read-only `lcs config status`, clearing observations at session teardown and
  marking preparation unavailable during reload/persistence. This is not the
  durable activation-readiness barrier; metadata alone cannot enable mutations.
- [x] Define bounded snapshot transfer, transaction messages, revision/digest
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
  use the explicit freeze and readiness workflows below.
- [x] Add distinct wire-6 readiness query/reply messages with strict codecs,
  established-peer and instance binding, monotonic attempt correlation, a fixed
  deadline, and collection from every configured member. INI-only nodes answer
  not-ready without creating files. Starting collection is still an internal
  coordinator operation; the automatic startup workflow invokes it after local
  frozen initialization.
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
  The activation manager and public mutation path invoke this proposer; agreement
  remains separate from resource application.
- [x] Pull committed checkpoint snapshots over admitted wire-6 sessions, binding
  requests to instances, static membership and frozen baseline. Validate original
  majority decisions relayed by any configured member, serialize persistence,
  and poll peers in rotation only after a durable barrier. Cover revision gaps,
  delete/recreate cleanup, quorum-only receivers, deadlines and disconnected writes.
  Learning remains separate from application and distributed startup admission.
- [x] Gate automatic activation on explicit capability and readiness confirmation
  from every configured member, including quorum-only nodes, with matching
  effective configuration. Unknown/offline members must not count as upgraded.
- [x] Add the same-process all-member first-initialization freeze collector and
  guarded writer entry point. Bind explicit receipts to the coordinator instance,
  monotonic attempt, exact baseline/membership, current upgraded peer instances,
  local reload generation and a fixed deadline. Revalidate sealed rounds before
  forking, consume authorization once, and reject incomplete member sets, stale
  replies, restarts, disconnects, changed definitions and withdrawn freezes.
  The peer exchange and live startup coordinator below use this collector;
  collection alone is not activation authorization.
- [x] Add the durable local freeze responder storage primitive on the shared
  writer. Persist only a format-3 FROZEN baseline before acknowledging; create
  no journals and never thaw/promote it on restart or collector timeout. Retain
  reload exclusion through uncertain writes and require exact all-member
  authorization to promote the retained runtime to format-2 initialization.
  Verify both freeze sync boundaries and all ten promotion sync boundaries with
  failures/crashes, immutable baselines, empty-history exclusion, and recovery
  before listeners. Explicit peer exchange, fenced restart resumption and automatic
  first-freeze/activation coordination are integrated below.
- [x] Add explicit wire-6 freeze query/reply framing and admitted peer dispatch,
  with destination/source instances, monotonic attempt correlation, strict codecs
  and complete baseline/membership binding. Route responder persistence through
  the coordinator's existing shared writer, count quorum-only members, and refuse
  storage while any configured member is offline/old or local reload is pending.
  Send LOCKED only after durable child publication and FROZEN only after drain;
  retain bounded replies across output
  backpressure, and drop stale/expired requesters without thawing durable state.
  Add the explicit scheduler-driven collector after local durable freeze. A
  collection-only entry does not initialize or switch authority; the separate
  initialization resumption workflow below explicitly authorizes local writes.
  Exercise real full/quorum-only daemons, wire-5 refusal, retries, mismatches,
  replaced instances, worker failure/shutdown and retained startup fences.
- [x] Restore healthy format-3 freezes before listeners for coordination-only
  startup, using their exact frozen definitions despite later valid INI resource
  edits. Retain resource voting/placement, reload and mutation fences; perform
  full-member startup cleanup only against the frozen baseline and no backend
  work on quorum-only nodes. Keep wire-6 coordination alive with advisory metadata
  while resource frames remain fenced. Report local phase frozen via diagnostic
  format 4 and export the retained baseline. Test stable all-member connections,
  repeated/quorum-only restart, INI edits/SIGHUP, absent history, unchanged durable
  intent and pre-listener rejection of contradictory records. The restore itself
  does not collect freezes, initialize history or activate JSON authority.
- [x] Resume already-durable freezes automatically through per-member all-member
  confirmation and guarded initialization on the coordinator's shared writer.
  Include quorum-only collectors; keep earlier pristine prepared nodes confirming
  their unchanged freeze so partial progress/restarts cannot deadlock collection.
  Preserve borrowed immutable base pointers across publication, keep prepared
  restarts read-only, and resume authorized interrupted INITIALIZING state. Refuse
  missing history, promises, reservations, barriers and advanced state rather than
  resetting them. Exercise full/quorum-only workflows, stale/replaced instances,
  shared-writer interruption/shutdown, all sync/crash boundaries, real three-node
  automatic preparation and mixed frozen/prepared/interrupted restart. First-freeze,
  readiness and activation are separate integrated stages below.
- [x] Automatically collect explicit all-member readiness after pristine local
  preparation, persist the sealed proof through the shared writer, and recover
  valid barriers over pristine revision-zero history without rewriting them.
  Retry only failed/expired in-memory unsealed collection with fresh attempts;
  persistence uncertainty requires recovery. Keep resource voting/placement,
  reload, JSON authority and mutation admission fenced. Verify stale replies,
  timeout retry, writer exclusion, full/quorum-only restart, unchanged proof and
  missing/corrupt history. Separate first-freeze and activation controllers are
  integrated below.
- [x] Resume the undecided revision-one activation slot after restart when a
  valid all-member barrier and mandatory request history exist. Preserve promises,
  reservations and accepted schema-2 activation values without rewriting records
  or resubmitting request IDs. Require the accepted value's exact reservation;
  retain resource, reload and mutation fences and frozen baseline cleanup. Cover
  lower-ballot rejection, missing proof/history, full/quorum-only restart and
  byte-identical evidence. Learned activation and initial application are handled
  by the recovery stages below.
- [x] Recover a learned schema-2 revision-one activation before listeners when
  its baseline remains applied, with either no pending application or that exact
  pending value. Keep mandatory history/barrier checks, clean retained baseline
  backends, obtain old-owner proof, publish/install the dynamic generation, FINISH
  with baseline predecessor history, then require fresh sync/quorum admission.
  Quorum-only members perform no backend work. Report baseline-applied recovery
  and cleanup failures using local diagnostic format 5. Test cleanup failure and
  crash/restart after BEGIN, absent history, baseline preservation, initial
  admission and retained learned decisions. Live activation uses the handoff below.
- [x] Hand a learned revision-one activation on a running frozen node to the
  recovery controller without restarting or resetting its coordinator, sessions,
  transactions, resource epochs, private promises or table indices. Wait for the
  shared writer and ordinary backend/lease work to drain; require the exact
  frozen baseline, resource fences and stopped/unowned tables before acquiring
  the application hold. Exercise busy writer/workers, missing fences, differing
  definitions, full/quorum-only paths, live checkpoint learning, unchanged daemon
  instances, majority edits with the witness offline and witness catch-up. Automatic
  freeze/proposal creation is integrated below.
- [x] Automatically propose the definitions-preserving schema-2 activation after
  an existing frozen node persists all-member readiness. Prefer the lowest-index
  connected full member; require an admitted wire-6 majority and retain fences
  through consensus/application. Allocate fresh instance/attempt request IDs,
  retain unknown reservations, and retry with higher ballots after deadlines.
  Correlated rejection hints may raise only the next ballot, never count as votes.
  Recover earlier majority-accepted values and refuse full/poisoned history rather
  than evicting evidence. Test isolated recovery, stale rejection hints, timeout
  retries, all-member initialization through automatic admission, accepted-value
  recovery, majority edits and witness catch-up. Warm INI first-freeze/drain uses
  the following integrated stages.
- [x] Drain a running INI generation after its durable local freeze, before
  acknowledging FROZEN or authorizing initialization. Fence resource placement,
  acquisition, renewal and incoming resource traffic before cancelling moves and
  lease operations. Use old hooks and dependency order; wait for backend/hook
  workers, residual grants, remote lease expiry and outstanding operations without
  resetting tables, epochs, private promises or administrative state. Failed
  cleanup stays frozen, reports its exact error and cannot produce a positive
  freeze receipt. Exercise active VIP removal, failed removal, repaired restart
  with unchanged intent, dependency/hook ordering, conflicts and quorum-only behavior.
  Automatic first-freeze initiation is integrated below.
- [x] Confirm a durable reload/acquisition lock from every configured member before
  draining any running INI owner. Keep exact unexpired lease renewals during lock
  collection, reject new/replacement grants, and refuse wire 5 immediately after
  durable locking. Use separate instance/baseline/generation-bound attempts for
  LOCKED proof and drained FROZEN receipts; LOCKED never authorizes initialization.
  Cover timeout/retry, stale replies, owner renewal, failed cleanup and restart.
  This prerequisite does not initiate the first freeze from ordinary INI startup.
- [x] Initiate the first durable freeze automatically from ordinary INI startup
  after every configured member, including quorum-only voters, has a current
  wire-6 session and matching complete-definition metadata. Require all members
  to finish legacy startup cleanup/lease-voting recovery; explicit responders also
  defer until local physical cleanup is verified. Exclude local reload
  and bootstrap refresh; accept matching peers already locked. Follow explicit
  all-member lock/drain/readiness proofs and majority activation before admission.
  Test real v1.1.1 rolling upgrade, offline/mismatching witnesses, conversion after
  coordinated reload, retained human INI and quorum-only restart. Keep pre-upgrade
  INI reload regressions on a current-source wire-5 fixture without a production
  bypass for the capability gate.

- [x] Connect the durable activation barrier to live INI reload and dynamic
  transaction serialization; version advertisements alone must not switch local
  authority.
- [x] Persist activation across restarts and catch up lagging nodes. After
  activation, use normal quorum requirements for edits rather than requiring
  all members to stay online; never automatically revert to legacy mode.
- [x] After activation, prevent older binaries or stale static configurations
  from rejoining as voters/owners under an incompatible resource definition.
- [x] Route configuration transfer and mutation through existing admitted-peer
  paths, including source allowlists, optional configured-peer restrictions,
  configured shared-secret and instance/version checks. Local commands use the
  existing Unix socket; this adds neither encryption nor a separate mutation ACL.
- [ ] Audit/document and, where needed, enforce Unix socket ownership/mode policy
  for foreground, daemonized and systemd execution. Test unauthorized local
  mutation attempts; inherited umask must not be assumed to be a sufficient policy.

## 5. Add runtime management commands

Definition commands below are available on admitted JSON full members after
supported automatic activation or recovery. Existing
`resource start/stop/move` remain runtime controls with their current semantics.

- [x] Model new resource/group definition commands on `pcs` syntax and conventions
  wherever practical, so administrators familiar with `pcs` can use familiar
  command structure, resource identifiers, option names, and `key=value`
  arguments for creation, modification, and deletion.
- [x] Before finalizing the CLI, compare the proposed commands with supported
  `pcs` commands and document equivalent LCS examples. Prefer familiar forms such
  as `lcs resource create`, `lcs resource update`, and `lcs resource delete` where
  their semantics match; preserve existing LCS commands and safety requirements.
- [x] Document unavoidable syntax or behavior differences explicitly, including
  LCS resource types, supported options, group semantics, and deletion rules.
  Do not imply support for Pacemaker resource agents or unsupported `pcs` features.
- [x] Add resource `create`, `update`, and `delete`, plus corresponding group
  operations, routed through one whole-snapshot transaction path.
- [x] Implement and unit-test the internal candidate editor for pcs-style resource
  create/update/delete/remove, group membership changes, and group policy updates.
  Route the public commands through the guarded agreement/application path below.
- [x] Define field replacement, explicit field removal, immutable fields, and
  rejection of invalid group/dependency deletions; avoid implicit cascading deletes.
- [x] Add offline `lcs config validate FILE|-` with selected INI static context,
  the shared strict JSON/candidate parser, bounded input, schema/incarnation and
  group/dependency checks, structured results and documented exit codes. Require
  no daemon connection and perform no writes, activation or resource operations.
- [x] Add atomic bulk import through the distributed transaction path. Stream
  bounded complete schema-1/2 documents with fixed upload deadlines; validate all
  definitions before reservation. Ignore imported authority metadata when assigning
  the next revision/backend identities from the committed predecessor. Bind retry
  identity to exact input bytes rather than filename, including after restart or
  later revisions. Exercise the full 512-KiB limit, malformed/expired/interleaved
  partial uploads, invalid whole candidates, stdin, empty replacement and retries.
  Verify disconnected/unsupported receivers preserve a structured unknown client
  result and the original request ID while the body is still being streamed.
  Read-only committed/applied export and offline validation remain separate.
- [x] Support expected-revision checks and idempotent request IDs; expose a
  transaction status command so interrupted clients can determine the outcome.
  Exact retries require local origin descriptor evidence; other nodes retain an
  unknown descriptor result and never resubmit. Automatic activation is separate.
- [x] Return actionable errors for no quorum, unsupported members, revision
  conflicts, invalid definitions, persistence failures, and failed resource stops.
  Preserve pending/unknown outcomes, include exact blocked-attempt backend or
  projection details and recovery instructions, and keep historical admission
  true despite later failures. Fatal shutdown attempts a bounded nonblocking
  diagnostic flush to existing waiters; lost transport still means unknown.
  Exercise failed backend stops, verified restart repair, reservation write
  failure before PREPARE, blocked exact retries and distinct older/newer scopes.
- [x] Provide structured `--json` results and consistent CLI exit codes.
  Exercise public VIP/group/policy/dependency edits through real daemon processes,
  old-base retries, descriptor conflicts, origin restart, quorum-only refusal,
  no-quorum rejection, blocked-admission pending and detached waiters. Reject INI
  submissions without initialization. Validate malformed/uncorrelated CLI replies
  as unknown, with the original request ID retained.

## 6. Support automatic upgrade and migration from existing releases

- [x] Test rolling binary upgrades from released v1.1.1 with existing INI
  resources and no dynamic files on the released nodes. Preserve owner/epoch
  during the mixed-version phase; after the final witness upgrade, verify
  automatic conversion and JSON admission. Conversion includes a resource drain.
- [x] Verify pre-activation rolling peering against the released v1.1.1 wire-5
  daemon: no dynamic files on released nodes, mixed wire-5/6 quorum, upgrading the
  other full member and quorum-only voter, unchanged active owner/epoch, and no
  new frames sent to old sessions. The compatibility test now also verifies
  automatic activation after the final upgrade.
- [x] Implement deterministic automatic bootstrap from effective INI definitions,
  preserving definitions, incarnation continuity and lease safety. Use compatible
  voting fingerprints and preserve placement during the mixed-version phase;
  drain/reacquire resources during the actual dynamic conversion.
- [x] Refresh prepared JSON after each successful legacy INI reload and startup.
  Perform runtime filesystem work in a background worker, coalescing refreshes
  without blocking lease renewal.
- [x] Serialize final conversion/activation against pending reloads and workers
  so an earlier prepared snapshot cannot replace newer agreed resource definitions.
- [x] Automatically activate dynamic authority after every member confirms
  capability, matching configuration, and durable readiness. No administrator
  migration command or configuration cleanup may be required.
- [x] Persist resumable initialization and activation metadata. Distinguish a
  never-initialized node from lost/corrupt state. Missing initialized shadow state
  fails closed; an activated resources.json projection is recoverable only from
  validated durable history, never by reimporting stale INI definitions.
- [x] Leave human-maintained INI files untouched and recover running definitions
  from the validated JSON catalog after activation. Report JSON authority and
  reject INI resource reloads while frozen/activated.
- [ ] Complete the retained-INI parsing and inactive-section diagnostic contract.
  Startup still parses/validates INI before JSON recovery, and reload rejection
  does not yet give explicit resource/group command guidance. Decide how stale,
  removed or invalid inactive sections and static daemon edits should behave.
- [ ] Define and implement safe provisioning/rejoin of a node without the required
  local dynamic history into an already activated cluster. Distinguish a new
  member from a previously voting node with lost/rolled-back promises; stale INI
  and an empty journal must not authorize participation. Membership changes remain
  outside this refactor.
- [ ] Document rollback before activation and a deliberate export/conversion path
  after activation. Downgrading binaries alone must not silently discard state.
- [x] Exercise retained frozen/initializing/prepared/accepted/chosen activation,
  interrupted storage publication, failed cleanup/repaired restart, and witness
  return in existing unit and process regressions. Preserve decisions and avoid
  implicit INI reimport or empty-history recreation.
- [ ] Expand migration failure testing into an end-to-end interruption/reboot
  matrix across rolling upgrades, activation/application boundaries and offline
  returns, checking duplicate backend creation and commit non-regression.

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
- [x] Unit-test revision conflicts, supported distributed journal recovery, and
  malformed/oversized wire transfers. Missing-history repair remains unsupported;
  corruption/missing mandatory records are tested as fail-closed cases.
- [x] Test the agreement core against competing coordinators, accepted-majority
  recovery after restart, duplicate/conflicting replies, corrupt/foreign journals,
  write/sync failures, old worker replays, cancellation, large worker results,
  and malformed/incomplete/expired snapshot transfers.
- [x] Integration-test public VIP create/update/delete/import, groups, policies,
  dependencies, quorum-only refusal/participation, origin retry/restart, and
  existing runtime controls (`dynamic-mutations.sh`, `config-import.sh`,
  `dynamic-live-application.sh`, `config-mutation-failures.sh`).
- [ ] Extend public dynamic create/update/delete/import coverage to systemd
  services and mixed VIP/service dependency transitions. Existing systemd worker
  and runtime-control tests are not this complete definition-command matrix.
- [x] Cover competing proposers and stale/duplicate/expired messages in unit
  transport harnesses; exercise public-command quorum loss, detached clients,
  origin restart, reconnects and definition remapping in process regressions.
- [ ] Extend the real-daemon matrix to simultaneous public edits, asymmetric
  partitions, coordinator changes, delayed/duplicate messages and stale reconnects
  during repeated remaps. Existing unit/harness coverage is not this full matrix.
- [x] Inject unit storage failures and process exits at activation/request journal
  sync boundaries; exercise blocked application, restart repair and uncertain
  writes in the existing process suite.
- [ ] Expand real-daemon crash injection across persistence and backend-transition
  boundaries, checking every acknowledged update survives and conflicting
  successors never commit.
- [x] Exercise supported full-cluster restart, retained stale checkpoints,
  corrupt/missing mandatory records and startup recovery with pending cleanup.
- [ ] Complete real-filesystem full-disk, permission-loss, stale-backup and
  interrupted-checkpoint/migration matrices, following the targeted runtime
  storage-isolation acceptance tests above. Existing validation/sync-failure
  injection does not substitute for those end-to-end conditions.
- [x] Verify legacy INI behavior during released-version mixed upgrades,
  all-member capability/definition/startup-readiness gating, reload before
  conversion, partial activation recovery, majority-based dynamic creation with
  the witness offline, and witness catch-up (`wire5-upgrade.sh`,
  `config-bootstrap-status.sh`, `config-initialization-recovery.sh`,
  `config-activation-live.sh`).
- [x] Document implemented runtime commands, storage, automatic migration and
  supported recovery in `docs/config.md`, `docs/manual.md`, `docs/resource-json.md`
  and `packaging/lcs.8`; record unreleased wire/local/storage changes in
  `docs/version.md`. Keep advanced operator detail out of the top-level README.
- [ ] Finish the packaged configuration/example and daemon-man-page audit, release
  notes and version-location updates for the eventual release. No release/version
  bump is implied by this reconciliation.
- [ ] Publish operator instructions for upgrade, migration, backup/restore,
  recovery, rollback, and package lifecycle behavior before releasing the feature.

Public mutation commands and automatic activation are implemented and exercised
by the current suite. Unchecked recovery/retention/policy work and the extended
fault/packaging matrices remain release-readiness work. Runtime availability under
long storage stalls is a newly explicit acceptance requirement and is not yet
verified; passing the existing suite must not be described as meeting it or
completing those broader matrices.


## Evidence used for this reconciliation

These pointers identify implemented behavior and existing regressions, not new
validation runs or completion of the remaining fault matrices.

| Area | Implementation | Existing evidence |
| --- | --- | --- |
| Automatic freeze/readiness/activation | [config_peer.c](../src/config_peer.c), [config_coordinator.c](../src/config_coordinator.c) | [config-bootstrap-status.sh](../tests/integration/cases/config-bootstrap-status.sh), [wire5-upgrade.sh](../tests/integration/compatibility/wire5-upgrade.sh), [startup-cleanup.sh](../tests/integration/cases/startup-cleanup.sh) |
| Authority, recovery and live application | [config_manager.c](../src/config_manager.c), [config_recovery.c](../src/config_recovery.c), [config_live.c](../src/config_live.c) | [dynamic-startup-recovery.sh](../tests/integration/cases/dynamic-startup-recovery.sh), [config-activation-live.sh](../tests/integration/cases/config-activation-live.sh), [dynamic-live-application.sh](../tests/integration/cases/dynamic-live-application.sh) |
| Public commands and local outcomes | [cli_server.c](../src/cli_server.c), [config_transaction.c](../src/config_transaction.c), [config_requests.h](../src/config_requests.h) | [dynamic-mutations.sh](../tests/integration/cases/dynamic-mutations.sh), [config-import.sh](../tests/integration/cases/config-import.sh), [config-mutation-results.sh](../tests/integration/cases/config-mutation-results.sh), [config-mutation-failures.sh](../tests/integration/cases/config-mutation-failures.sh) |
| Agreement, storage and transport | [config_agreement.c](../src/config_agreement.c), [config_transfer.c](../src/config_transfer.c), [config_store.c](../src/config_store.c) | [config_agreement.c unit tests](../tests/unit/config_agreement.c), [config_transaction.c unit tests](../tests/unit/config_transaction.c), [config_activation.c unit tests](../tests/unit/config_activation.c), [config_requests.c unit tests](../tests/unit/config_requests.c) |
| Admission and access policy | [peer.c](../src/peer.c), [lcsd.c](../src/lcsd.c) | [config-freeze-exchange.sh](../tests/integration/cases/config-freeze-exchange.sh), [config-readiness.sh](../tests/integration/cases/config-readiness.sh), [source-allowlist.sh](../tests/integration/cases/source-allowlist.sh), [peers-only.sh](../tests/integration/cases/peers-only.sh); foreground socket-mode policy still needs audit |
| Package directory creation | [Makefile](../Makefile), [lcs.postinst](../debian/lcs.postinst), [package-rpm.sh](../packaging/package-rpm.sh), [lcsd.service](../packaging/lcsd.service) | State-directory creation/ownership is present; package lifecycle matrix remains unchecked |
