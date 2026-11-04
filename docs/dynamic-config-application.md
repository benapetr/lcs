# Configuration application and recovery contract

Status: contract with implemented local catalog and reconciliation sequencing;
live daemon backend adapters and automatic activation remain unfinished. The running daemon
still uses INI authority and supports wire 5–6. Wire-6 bootstrap metadata does
not enable dynamic edits.
See [the agreement core](dynamic-config-agreement.md) and
[implementation checklist](dynamic-config-plan.md).

## Resource identity and message admission

A resource name is its administrative identity. Its backend incarnation is the
pair `(name, birth_revision)` within the activated configuration history.
Bootstrap resources have birth revision zero. Creation and replacement of a
backend allocate the revision in which that incarnation first appears.
Deleting and recreating a name must allocate a new birth revision even when its
backend properties are identical. Changing a VIP address/interface, service
unit, or resource type is a backend replacement. Policy, priority, dependency,
group and hook edits preserve incarnation, subject to safe application below.

The committed dynamic snapshot must carry these identities, including during
full-snapshot catch-up. Inferring identity from just the old and new endpoint
definitions is insufficient: a node can miss deletion and recreation between
two identical definitions. The existing schema-1 shadow JSON has no incarnation
metadata. The implemented schema-2 library carries and validates these identities;
daemon activation remains unfinished; explicit generation-bound resource framing
is now implemented below.
The coordinator allocates identities; acceptors check them against the immediate
predecessor. Clients cannot supply or reuse birth revisions.

Resource array indices are local sorted-table offsets, not durable identities.
For active dynamic sessions, every resource message must be bound to the applied
configuration digest and incarnation, either in a new envelope or an explicitly
negotiated immutable session context. This includes lease requests/replies,
renewals, release, state sync, administrative controls and delayed worker results.
An index without a matching context cannot address a resource. Closing sessions,
cancelling indexed operations and discarding queued frames precede remapping.
Wire-5 sessions may be used during legacy upgrade, but cannot vote or host under
dynamic authority. A reconnect must not downgrade the authority or feature gate.

Preserve runtime ownership/grants only for a matching incarnation and backend.
Configuration revision changes alone do not increment resource epochs. New
incarnations start without ownership or inherited grants, and old messages cannot
populate them. Dependencies are resolved again against the new sorted table.

## Implemented resource-generation framing

Wire 6 now reserves `RESOURCE_FRAME` (37), a format-1 envelope around resource
requests and replies, including lease/state-sync/move traffic and voting
heartbeats. The fixed 158-byte prefix carries inner type, both daemon instances,
applied revision, static membership digest and the complete applied value digest.
The value digest includes persisted incarnation metadata, so identical backend
properties after delete/recreate cannot reuse an old generation. The outer RPC
sequence is retained. Configuration readiness/agreement/checkpoint messages stay
outside this envelope and remain available between different resource generations.

Admission runs before resource dispatch, state import or RPC completion. It
requires negotiated wire 6, the admitted peer's instance, this daemon's instance,
a valid resource inner type and canonical digests. One socket can advertise only
one immutable generation. A malformed envelope or an in-place generation change
closes that connection. A well-formed generation mismatch drops resource traffic
and clears peer voting/recovery-sync eligibility, while retaining configuration
transport so catch-up can proceed. Unwrapped resource traffic cannot restore a
socket that has advertised a generation. An INI-authoritative receiver cannot
consume wrapped resource state. HELLO's voting-ready bit cannot enable a dynamic
peer; matching wrapped heartbeat/state traffic is required.

For explicitly installed dynamic authority, HELLO retains its released layout but
uses the frozen baseline's resource/group counts and fingerprints. Static identity,
roles, timing, source-address and secret checks remain in place. Peers with the
same baseline but different current resource definitions can therefore connect
for configuration synchronization. Legacy reload proofs cannot mutate dynamic
state, and wire-5 sessions are rejected under dynamic authority. Legacy INI
sessions otherwise retain their existing layouts and behavior.

`resource_generation.c` provides explicit install/admit entry points for the
future live reconciler adapter. Installation requires a durable pending/applied
snapshot and valid barrier, and refuses active or queued indexed backend/hook/
probe/handoff work in any slot, including removed resources. It fences generation
traffic and voting, cancels move/lease operations, and closes peer/handshake
sessions BEFORE the caller remaps indices. Old-owner cleanup and correct runtime
remapping remain the adapter's responsibility; this helper does not prove them.
Reinstalling the exact same context is idempotent; regression/conflict is rejected.
There is no API to silently revert an installed generation to legacy mode.

Admission requires the exact durable applied snapshot with no pending catalog,
no configuration writer in flight, and a complete matching installed resource/
group definition table. It enables generation traffic, then ordinary lease restart
recovery still controls local voting. Mismatched peers cannot contribute votes or
restart state-sync receipts. Resource RPCs are refused before registration until
a peer's generation matches. Initial wrapped heartbeats/state-sync may establish
that match; queued messages retain the existing bounded transport/backpressure
rules. The 64-KiB frame limit includes the prefix.

These APIs are not invoked automatically yet. The daemon still refuses persisted
distributed startup. Backend/owner adapters, runtime remapping, startup projection
and recovery admission, and automatic activation remain unfinished. Tests cover
strict codecs, stale instances and generations, inner reply/sequence preservation,
legacy refusal with an intact config channel, worker/marker admission barriers,
and cancellation-before-remap ordering. They do not yet constitute a running
dynamic-cluster backend transition test.

## Decision, drain and application ordering

1. Validate the complete candidate and expected predecessor, including identities,
   dependencies and static membership. Serialize activation, INI reload and dynamic
   edits; only one may transition local resource definitions at a time.
2. Run durable prepare/accept agreement. Persist promises and accepted values
   before their replies. No backend changes occur merely because a proposal was
   received or an acceptor voted. Once a value is chosen it cannot be rolled back
   because application failed; finish learning and repair application instead.
3. Persist the learned decision before reporting it committed. Publish
   `resources.json` as a projection of that decision. Do not start new backends
   yet. A projection write failure leaves a committed but pending operation.
4. Before any local backend operation, persist the last-applied definitions,
   incarnations and the complete cleanup obligations for the transition. This
   catalog must include the old backend details and stop hooks, rather than only
   resource names. Retain it until cleanup and an applied marker are durable.
5. Suspend grants/acquisition for affected resources and their transitive
   dependents. Stop dependents before dependencies, using the old definitions
   and stop hooks. Backend replacement and deletion require cleanup. Dependency
   edits require dependency readiness to be reevaluated before dependent restart.
   Policy/group changes may require placement reconciliation. Stop completion,
   outstanding hooks, probes, handoffs and backend workers are part of the barrier.
6. Establish that every old owner affected by the transition has stopped, or
   that its old lease has expired under the existing lease timing/recovery rules.
   A disconnected owner does not count as stopped. A known or suspected failed
   backend stop requires confirmed cleanup or external fencing; waiting alone
   cannot prove removal of an address or termination of a service. Keep affected
   activation blocked on `stop_failed`; do not override it through a majority.
7. Remap runtime state only after the local drain and old-generation ownership
   barrier. Preserve unaffected incarnations and rebuild all index references.
   Persist the applied snapshot/marker before admitting normal votes under the
   new generation. Start eligible resources in dependency order under new leases.
8. Mark the request applied once the coordinator's application barrier has
   succeeded: every participating online member has applied, and excluded owners
   have satisfied step 6. Applied means definitions installed and cleanup complete,
   not that every service health check succeeded or every resource is running.

The existing INI reload retains its existing all-participant drain-before-apply
behavior during rolling upgrade. It does not become a durable dynamic transaction
by reusing its in-memory fingerprints. Shared validation and transition planning
must not silently weaken its current barriers.

## Restarts, catch-up and missing files

| Durable state at interruption | Required recovery |
| --- | --- |
| Promise or accepted value, no known decision | Recover agreement; a replacement proposer must carry forward any chosen value. Do not assume abort. |
| Decision committed, projection stale/missing | Verify the local journal/history and rewrite the projection; never reload resource authority from INI. |
| Cleanup catalog persisted, backend action interrupted | Retry/probe cleanup conservatively using recorded old definitions. Do not vote or host affected resources until resolved. |
| Cleanup finished, applied marker not persisted | Repeat idempotent reconciliation; do not infer application from a missing backend alone. |
| Applied marker persisted | Recover the matching incarnation catalog, perform ordinary lease restart recovery, then rejoin resource voting. |
| Known active journal or cleanup catalog missing/corrupt | Enter recovery-only mode with an actionable diagnostic. Obtain verified state and reconcile/fence local backends; never initialize empty state automatically. |

Full-snapshot catch-up may skip committed revisions for definitions only. It
must preserve local cleanup obligations from the last applied catalog, and use
incarnations carried in the destination snapshot. It cannot discard an unfinished
cleanup catalog when a newer snapshot arrives. Application is serialized locally;
configuration synchronization does not itself admit resource votes or ownership.
Quorum-only members retain decisions, incarnations and applied metadata, but
have no local backend operations to perform.

Ordinary full-cluster restart recovers each node's monotonic agreement records
before seeking a matching majority and admitting resource activity. Revisions
alone cannot establish authority. Historical decision compaction must retain the
checkpoint's verified predecessor/decision evidence and the state needed for
unfinished requests and cleanup. The committed checkpoint path now supports
multi-revision catch-up; durable request-status history and compaction remain
unfinished.

Disk replacement or restoring stale backups is disaster recovery, not an ordinary
restart. A node with lost/rolled-back promises must remain excluded from votes
until an explicit recovery procedure establishes a safe new voting state. Do not
allow a former minority or stale restored majority to overwrite current history.
Automatic repair of lost agreement state and membership reconfiguration remain
unsupported; operators must retain a verified majority's durable state or stop
and fence the cluster before an explicit history reset. Backup/restore tooling
must capture journal and application catalogs together, never only resources.json.

## Implemented local application catalog

`config_application.c` implements the local durable catalog. Daemon startup now
loads existing agreement and application records before considering INI snapshot
refresh. An existing `activation.json` intent first restores the frozen baseline
and resumes interrupted local journal/catalog initialization. After local preparation,
missing records require recovery rather than recreation. This does not authorize
configuration voting or activate JSON authority. It retains an immutable base for
both handles and refuses to run persisted distributed state until activation-aware
peering/application exists.
The catalog is not yet connected to live resource application; its synchronous
primitives are available through the same background storage worker used for
agreement. It uses the same exclusively locked state directory as agreement and
a node/membership-bound,
checksummed `application.json` record. The record contains canonical complete
last-applied and optional pending configuration values, including incarnations,
backend properties and hooks. It does not contain lease ownership or epochs.

A missing catalog may only be initialized with explicit authorization while the
agreement is still at revision-zero bootstrap. It cannot be recreated from a
current active snapshot. Begin accepts only a decision already learned by the
local agreement handle, and persists old plus pending definitions before any
backend operations. Repeating the same begin is idempotent; a different newer
decision cannot overwrite an unfinished pending catalog. It must wait until
cleanup and completion are durably recorded. After that, a newer full snapshot
can be applied directly without forgetting local cleanup history.

The planner maps matching name/incarnation/backend identities across sorted
arrays. Deleted or replaced incarnations require old-backend cleanup; changes to
dependencies require drain, including transitive dependents. Backend identity
conflicts under one incarnation and reuse of an incarnation absent at the applied
revision are rejected. Policy/group placement reconciliation and actual
stop/start execution remain responsibilities of the future daemon coordinator.

Finish requires the exact pending digest and the caller's completed cleanup and
old-owner barrier. It persists pending as applied and removes the old snapshot in
one atomic record replacement. Finish itself does not stop a backend or prove
that cleanup occurred. Failed persistence poisons the handle; reopen from disk
before further work. A directory-sync failure after rename is uncertain, so
recovery may observe either old/pending or completed state. Missing, corrupt,
foreign or agreement-conflicting records fail rather than resetting authority.
Tests cover restart mid-transition, newer decisions arriving during cleanup,
catch-up over deletion/recreation, dependency drains and failed durability.

The shared worker serializes agreement and application writes. It persists begin
or finish in the child and transfers a bounded, checksummed catalog back to the
parent. Parent import performs no filesystem I/O and accepts only the unchanged
catalog, staging the latest learned decision without dropping old definitions,
or completion of the exact pending application. Replays that erase cleanup
obligations or regress the applied revision are rejected. Cancellation or an
uncertain worker result poisons both handles until disk recovery after child exit.
Tests cover shared-writer exclusion, delayed publication, large pipe results,
stale imports, failed file/directory sync, and cancellation. The internal
reconciliation sequencer below now coordinates catalog writes and explicit
backend barriers; live daemon adapters remain pending.

## Internal reconciliation sequencer

`config_reconcile.c` implements the local ordering state machine. It requires an
explicit start after durable activation authorization and startup fencing; no daemon backend
adapter, automatic authority switch or permissive default callbacks are installed.
Projection publication, live adapter calls to the generation binding APIs, and
startup ownership recovery still need daemon integration before this path can run live. An already
current catalog returns done without adapters; callers must separately establish
ordinary restart and generation admission, including a crash after applied
persistence but before the prior process completed admission.

One reconciler takes a coordinator ownership hold that blocks configuration votes
until admission finishes. Catalog BEGIN/FINISH use the same background writer as
agreement/checkpoint operations, with separate application completion counters so
a catalog result cannot be mistaken for a peer transaction vote. BEGIN must be
durable before any adapter is invoked. Recovery of an existing pending catalog
uses its exact target even if newer decisions have already been learned.

The required nonblocking adapters execute in this order:

1. Quiesce affected grants/acquisition while preserving eligible unaffected work.
2. Drain local backends and indexed workers using old definitions/hooks and safe
   dependency ordering; failed stops block application.
3. Establish the old-owner barrier through confirmed stop or valid lease expiry;
   known/suspected failed cleanup needs confirmed repair/fencing.
4. Install the target while keeping resource activity fenced. Close old indexed
   sessions and operations, then remap only matching name/incarnation/backends.
5. After the exact installed target is durably recorded as applied, establish
   generation/restart/lease admission before releasing the hold.

Adapters return pending, complete, or blocked. A completed step must maintain its
guarantees until admission or shutdown. They may not block the lease event loop,
retain borrowed plan pointers, or reenter/mutate the coordinator or durable
handles. Asynchronous results must bind to the immutable target digest/incarnation
and monotonically increasing reconciler attempt. Adapters copy needed data; late
results cannot stand in for another attempt. Quorum-only nodes skip the three
backend/owner steps, while still installing metadata and completing admission.

A failed adapter preserves the exact stage, error and cleanup target in blocked
state. It advances only after an explicit retry following external repair;
no elapsed timeout proves cleanup or admission. Uncertain persistence is terminal
recovery state and cannot use that retry path. Both kinds of failure retain the
voting hold. An applied marker alone cannot enable votes while admission is
pending or failed.

Checkpoint learning may advance during drain. It cannot overwrite the pending
application. The sequencer finishes that target, then stages the latest decision
without admitting the intermediate generation. It also waits for any in-flight
checkpoint before admission, preventing the old generation from being admitted
while a newer decision is being imported. Stop/install callbacks are trusted
local safety barriers, not distributed owner acknowledgements created by this
module. Live adapters must provide the actual peer, backend and lease checks.

Shutdown stops the shared coordinator and retains storage and snapshots until its
writer is reaped. Closing a live owning reconciler is refused. Adapters must also
cancel/drain their asynchronous work before disposal. A restart resumes from the
durable pending catalog and repeats idempotent barriers, rather than interpreting
an interrupted callback or absent process as successful cleanup. Tests exercise
ordering, failed stops, pending owners, failed generation admission, newer
checkpoints during drain/admission, quorum-only paths, uncertain BEGIN/FINISH
writes and shutdown while a writer is stopped at fsync.

## Client outcomes

Every mutation carries an idempotent request ID and expected base revision/digest.
Persist request identity and outcome as part of decision/status history before
acknowledging it. Retrying the same ID with different content is a conflict.
Recovering someone else's previously accepted value does not commit the waiting
client's edit; report that edit's own status or predecessor conflict.

New mutation/status endpoints use these results (existing runtime commands keep
their current exit codes):

| Result | Meaning | Exit status |
| --- | --- | --- |
| `applied` | Decision is durable and its application barrier completed. | 0 |
| `rejected` | Request was not admitted, or a durable final outcome establishes it did not commit. Include a reason such as validation, stale base, unsupported members or no quorum. | 1 |
| `pending` | Request is known; agreement or application has not finished. Include committed revision when known and any cleanup failure. | 2 |
| `unknown` | The responder cannot establish the request's outcome, including client timeout/disconnect. This is not an abort. | 3 |

Structured output includes request ID, result, committed revision/digest when
known, applied revision and an actionable reason. A known committed request with
failed cleanup stays `pending`, rather than becoming rejected. A timeout before
decision observation is `unknown`; query/retry the same request ID rather than
blindly submitting a second edit. Status retention/compaction must make expired
request IDs explicitly unknown, never falsely rejected or reusable within a
retained history.
