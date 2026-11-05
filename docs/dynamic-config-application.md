# Configuration application and recovery contract

Status: contract with implemented local catalog and reconciliation sequencing;
local backend callbacks, receipt retirement and explicit startup cleanup are
implemented, including completed scopes backed by retained predecessor history.
Differing-generation/missing-history catch-up, daemon integration and automatic
activation remain unfinished. The running daemon
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
distributed startup. The local runtime adapter below now implements backend/remap callbacks;
missing-history catch-up, startup integration and automatic activation
remain unfinished. Tests cover
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
last-applied and optional pending configuration values, plus the two preceding
complete applied snapshots, including incarnations, backend properties and hooks. It does not contain lease ownership or epochs.

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
old-owner barrier. In one atomic record replacement it promotes pending to
applied, shifts old applied into history slot 0 and old slot 0 into slot 1,
and drops only the snapshot outside that bounded window. Finish itself does not stop a backend or prove
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
or completion of the exact pending application with the exact predecessor-history
rotation. Replays that erase cleanup/history obligations or regress the applied
revision are rejected. Cancellation or an
uncertain worker result poisons both handles until disk recovery after child exit.
Tests cover shared-writer exclusion, delayed publication, large pipe results,
stale imports, failed file/directory sync, and cancellation. The internal
reconciliation sequencer below now coordinates catalog writes and explicit
backend barriers; the local adapter below now supplies backend/remap callbacks, while its
missing-history catch-up and automatic daemon integration remain pending.

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

## Local backend drain and runtime adapter

`config_live.c` now supplies explicit local quiesce/drain/install/admit callbacks
for the reconciliation sequencer. It is not started automatically. Initialization
requires both a projection publisher and a distributed old-owner barrier provider;
there are no permissive defaults. The publisher receives the latest learned value,
which may be newer than a pending application, and must complete an idempotent
asynchronous publication. The owner provider must establish generation-bound
stop, lease-expiry or fencing evidence. Gossip and a timer alone are not that
proof. Unexpected positive provider statuses block the transition.

Quiesce verifies the live table against the durable last-applied catalog and the
target against pending state. It cancels moves and affected lease operations,
fences new acquisitions and renewals for changed/dependency-affected resources,
and leaves releases available. Late acquisition, hook, VIP probe/add and service
start completions cannot activate a fenced definition. Administrative starts and
moves are refused while affected definitions are being reconciled.

Drain calls the existing local backend stop path with the OLD definition and stop
hooks. Dependencies wait for their dependents to stop AND finish post-stop work.
Active/queued hooks, probes, VIP/service workers and handoffs remain part of the
barrier. Known cleanup failure blocks for explicit repair/fencing; it is not
cleared by time or a new configuration. Local drain is rechecked before both the
owner provider and installation, so a late failure cannot be overlooked.

Installation fences indexed resource traffic, waits for the generation helper's
all-worker barrier, and cancels peer/lease/move operations before copying runtime
state. Only matching name, birth and backend identities retain runtime ownership,
leases, private promises and administrative state across sorted index changes.
New/replaced incarnations start stopped, ownerless and with empty epoch/promise
state; changing a configuration revision does not increment unchanged epochs.
Dependency indices come from the validated new table. A pending earlier target
continues to completion before a newer checkpoint can replace its cleanup work.

During the brief remap/admission pause, owned unchanged resources may keep only
their EXISTING valid lease. No missing quorum/admission can extend it. Expiry
still invokes the ordinary backend drop path and known stop failures remain
unsafe. Background health work does not restart indexed workers during that
pause. After the exact applied marker is durable, the adapter admits generation
traffic so ordinary state-sync/restart recovery can run. It releases the remaining
definition fences only after local voting readiness and quorum are established.

The running daemon still lacks automatic owner-provider integration, the startup
reconstruction/cleanup adapter and automatic activation wiring. The explicit
network provider below supports sequential transitions with all-member receipt
retirement and a retained completion scope.
This live adapter assumes the last-applied backend/ownership state was recovered;
ownerless runtime entries alone do not establish that old backends are absent.
Tests exercise local callbacks with controlled backend/provider doubles, including
old stop hooks, dependent post-stop work, failed cleanup, malformed provider
status, mandatory barriers, sorted remapping, retained ownership/promises,
replacement identity reset, lease expiry and generation admission. They do not
yet constitute an end-to-end dynamic-cluster backend transition test.

## Durable projection publisher

`config_projection.c` supplies the concrete asynchronous publisher used by
`lcs_config_live_init_with_projection()`. It borrows the bootstrap snapshot
directory and the reconciliation coordinator; these must remain open until the
shared worker exits. The distributed owner barrier is still a required caller
provider. Neither initializer starts automatic daemon application.

Publication shares the single storage worker with configuration agreement and
application markers. Only the latest learned dynamic value may be submitted,
after frozen initialization and the durable activation barrier. The child writes
the exact schema-2 document, including revision and incarnation metadata, using
private mode-0600 replacement, file fsync, rename and directory fsync. Parent
polling performs no filesystem I/O. Agreement and application records remain the
authority; `resources.json` is a derived projection, never a new decision.

Completion identifies the published revision and value digest. If a newer
checkpoint arrived before the callback observes completion, it schedules the
latest projection before reporting success. Repeated calls for the same completed
value require no new write. A known projection file error blocks application and
clears that receipt, while retaining healthy agreement/application handles for
retry after repair. A failure after rename may already have replaced the file;
retry rewrites the authoritative value. A cancelled, broken or uncertain worker
retains the existing conservative recovery requirement and must be reaped before
storage closes. Startup must rewrite/verify projection from recovered authority;
an in-memory publication receipt does not survive restart.

Tests cover shared-writer exclusion, delayed fsync, exact contents and permissions,
superseded completion, retry after directory fsync failure, symlink rejection,
cancellation lifetime, and publication through the local application adapter.

## Old-owner proof primitives

`config_owner_barrier.c` provides an internal stop-proof collector and responder.
The explicit network adapter described below now connects it to admitted peer
transport and local drain callbacks. Exact all-full-member application confirmation
now retires receipts across sequential transitions. Automatic
application remains disabled. Transport authenticates the configured source,
requires wire 6, copies outgoing messages, and reports session/instance changes
before installation.

A scope comes from the healthy durable applied/pending catalog: both complete
value digests, old and target revisions, static membership, and the dependency
closure that must drain. The digests bind resource identities, definitions and
hooks; a matching resource name or revision alone is insufficient. Queries and
replies add sender/target daemon instances and a strictly increasing round ID.
The codec rejects truncated/extra bytes, invalid revisions, zero instances/IDs,
invalid statuses and noncanonical digest fields.

Every configured full member, including the requester, must explicitly confirm
local stop. Unavailable full members remain required. Quorum-only members do not
host backends and cannot contribute owner proof. A quorum decision, gossip,
PENDING response, disconnection or elapsed timeout cannot substitute for stop.
Timeout fails the round; explicit retries use a new ID and collect evidence again.
A changed/lost full-member instance or contradictory current reply invalidates
even a completed round. The caller must observe sessions and recheck the barrier
immediately before installation.

A local receipt may be sealed only after known restart cleanup and the complete
local drain, including stop/post-stop hooks and outstanding workers, while old
generation grants/reactivation remain fenced. The primitive does not discover or
perform those backend actions. It pins one exact scope and instance; a query
never creates a receipt or changes its scope. Missing receipts answer PENDING,
conflicting scopes or known failure answer FAILED, and revocation is terminal.
A receipt cannot be reused after a daemon restart or silently evicted to make
room for another application. The network provider retains the current scope until
every full member confirms exact durable application, then retains its completion
record for slow consumers as described below. Restart reconstruction is unfinished.

These primitives deliberately block different old-generation scopes. Applying
checkpoints across revision gaps needs additional cleanup-scope reconciliation;
a higher revision alone does not prove that an intermediate backend was stopped.
No expiry-only or external-fencing success path is implemented here. These
limitations keep automatic application disabled. Tests cover durable scope
extraction, two full members plus a quorum-only member, refusal of a mere
majority among three full members, offline peers, stale/duplicate/contradictory
replies, instance changes after completion, pinned/revoked receipts, fixed
deadlines and malformed codecs.

## Explicit owner network adapter

`config_owner_network.c` connects the stop-proof primitives to bounded message
exchange. `CONFIG_OWNER_BARRIER` (38) is a separate wire-6 configuration message,
with the existing strict 243-byte format-1 query/reply body. It stays available
while resource generations differ and never uses indexed resource envelopes.
Peer dispatch and queueing require established, admitted wire-6 full-member
sessions; quorum-only members neither send nor contribute owner proof. Both
queries and replies use fresh outer sequences, with correlation in the body.

Queries never start cleanup or create a receipt. Legacy INI-only nodes answer
PENDING without initializing distributed history. There is one copied pending
response per peer; failed queue attempts retain it for retry, and a local
revocation changes unsent STOPPED responses to FAILED. Session loss discards
buffered responses and invalidates a collection bound to that instance, including
a completed collection. Query retries run at most every 250 ms and retain the
original ten-second collection deadline. A transport failure cannot reduce the
required full-member set. A valid FAILED response blocks the barrier without
being treated as malformed traffic.

`lcs_config_live_init_with_owner_network()` explicitly binds the concrete
projection publisher and owner callback to the same recovered base/catalog.
After local drain succeeds, the callback derives the exact catalog scope, seals
the pinned local receipt, and starts collection. The live adapter checks local
drain and owner proof again immediately before installation. A newly observed
cleanup failure revokes the local receipt and blocks network-only retry. A failed
collection may be explicitly retried under a new round ID while retaining its
local stop receipt and fences; neither timeout nor a retry repairs failed cleanup.
Transport/session processing must precede reconciliation so the final check sees
current admission. Normal installation closes indexed sessions only after this
check; other collectors may need to retry after those intentional reconnects.

A new scope waits while the previous receipt is pinned, including when a newer
checkpoint becomes pending during application. The all-full-member retirement
exchange below permits the next scope after the previous application is confirmed.
Differing-old-generation recovery and startup cleanup remain unfinished. Creating
a fresh network object to discard required receipts is not a supported recovery
action. The daemon does not automatically initialize or run the local adapter.

Tests exercise three full-member transport participants, delayed local drains,
PENDING responses, bounded output backpressure, fixed deadlines, instance changes,
explicit retry, failed-response delivery, refusal of a later scope before retirement, and final
installation checks after disconnect/cleanup failure. Socket integration tests
verify the actual frame dispatch, INI-only PENDING replies, wire-5 rejection,
wrong-instance rejection, and absence of initialized distributed records.

## Receipt retirement across transitions

The same wire-6 frame now carries APPLIED_QUERY (kind 3) and APPLIED_REPLY
(kind 4). Their scope is identical to the stop phase: exact old/target value
digests and revisions, membership and drain mask. Result 1 means APPLIED in a
kind-4 reply. Each phase has a distinct monotonic round ID. STOPPED responses
cannot count as APPLIED evidence, and unsolicited/mismatched replies cannot
initialize or release a receipt.

A member begins retirement only after its healthy durable application catalog
records the exact target digest and revision as applied. A pending marker,
backend callback, stop acknowledgement or larger revision is insufficient. It
can then acknowledge its own application and collect exact confirmation from
every configured full member. Quorum-only members do not host the old backends
and are excluded from this cleanup lifetime barrier. The complete configured
full-member set remains required across outages. Failed/conflicting evidence,
session changes or the fixed deadline leave the receipt pinned; explicit retries
use a new round ID and collect again. Failed cleanup or poisoned application
state cannot be repaired with a network retry.

Once all confirmations arrive, the provider retires the active stop receipt and
retains its exact scope as a completion record. A slow peer may still be collecting
APPLIED confirmations for that scope. The retained record answers its query even
while the fast member's catalog is pending the next transition. Queries for a
future transition receive PENDING while the current receipt is pinned; they do
not displace it or count as application confirmation.

The next scope must begin at that exact completed target. Its completion record
replaces the preceding record only after every full member confirms application
of the next scope. Each such member had to retire the preceding scope before
sealing its next stop receipt, so no surviving collector still needs the older
record. This ordering bounds memory to one active receipt plus one preceding
completion record, without timer eviction or treating revision order alone as
backend cleanup proof. A failure in a later transition does not erase a prior
completed record. All instances and digests remain bound to the query/reply.

These records are process-local proof. A restarted daemon cannot recreate them
from an applied revision alone; it answers PENDING for an unknown scope. The
startup cleanup runner below can recreate pending and completed scopes under a
new instance only after physical cleanup of the retained snapshots. Missing
predecessor history and differing-old-generation reconciliation remain unfinished;
automatic application remains disabled. Tests cover three sequential transitions with
independently published durable catalog records, a delayed local application,
lost completion replies, a fast member serving a slow peer while on its next
transition, phase confusion, conflicting digests, session loss, timeout/retry,
poisoned catalogs and refusal to infer proof after restart.

## Explicit startup backend cleanup

`config_startup.c` now supplies a concrete, explicitly invoked local cleanup
runner. It requires recovered frozen authority, an intact activation barrier and
a healthy applied/pending catalog. It does not enable the daemon's distributed
startup path; normal startup still refuses persisted distributed history until
the remaining recovery/admission manager is integrated.

The runner copies and binds the complete last-applied snapshot, its two retained
predecessors and any pending value. It cleans **every** backend in the applied
and retained historical snapshots on this node because ownership is unknown
after a process restart. It processes the current table first, then each older
table, with dependents first within each table. Even an empty current table can
have historical backends that require cleanup. It does not use the edited INI definitions or
replace old cleanup with the pending/newest target. Before FINISH, new definitions
cannot start backends, so the retained applied snapshot covers possible active
backends; after FINISH, the applied catalog already names the installed target.
This relies on the application's existing no-activation-before-FINISH ordering.

Cleanup runs dependents before dependencies, using each applied definition's old
pre-stop hook, VIP deletion/systemd stop, and post-stop hook. It waits for each
hook/backend worker to exit before proceeding. A pre-stop failure still attempts
backend removal and post-stop, but prevents a successful recovery result. Failed
backend removal, failed post-stop or timeout blocks for explicit repair/retry;
it never turns into proof through elapsed time. Hooks must be idempotent,
foreground commands because recovery can repeat them with ownership unknown.

The runner uses the existing backend async workers; parent polling performs no
filesystem I/O or blocking backend calls. Backend operations use at least the
existing five-second bound (or the configured hook timeout if longer); hooks
use the configured hook timeout. Cancellation retains worker identity and
snapshot storage until reaping. A changed/poisoned catalog invalidates completion,
cancels outstanding work and requires recovery rather than ordinary retry.
Quorum-only nodes skip backend work. Empty applied tables need no backend work.

Recovery hooks use the ordinary resource/type/address/unit/event environment,
plus `LCS_RECOVERY=1` and `LCS_CONFIG_REVISION` naming the applied revision.
`LCS_EPOCH` and `LCS_LEASE_ID` are zero: cleanup does not fabricate ownership or
private voter promises. VIP, systemd and hook process groups are bound to their
launching daemon. Hook programs execute under a group supervisor so the hook's
foreground child commands are cancelled on daemon death as well as on timeout;
workers do not retain daemon listeners or storage descriptors.

Successful cleanup is checked against the same catalog and the new daemon
instance. When predecessor history is available,
`lcs_config_startup_restore_completed()` restores the latest completed scope and
its preceding completion record from exact saved snapshot pairs. It restores
only local proof under the new instance, never old replies or an all-member
certificate. Fresh all-full-member application confirmation must retire that
scope before `lcs_config_startup_restore_pending()` can recreate a pending stop
scope. Applied revision numbers alone and discarded history are insufficient. Completion leaves
`requires_recovery` set and does not write journals/catalogs, install runtime
ownership, admit a resource generation, or enable lease/configuration voting.
The caller must keep activity fenced, perform ordinary lease/restart recovery,
and integrate the authoritative runtime table before admission. Close the runner
only after all its workers are reaped; its decoded snapshot is owned until close.

Tests reopen durable history with conflicting INI definitions and a newer pending
deletion, verify current/historical backend and hook details and dependency order,
and recreate exact pending/completed proof under a new instance. They cover service/VIP/hook failures, explicit
repair/retry, timeouts, cancellation, poisoned catalogs, completion invalidation,
quorum-only skips, refusal to invent missing predecessor history, and daemon death with
backend/probe workers and hook child commands still running. These use real worker
processes with dry-run backends; they do not constitute live daemon recovery.

## Durable completed predecessor history

Application catalog record format 2 adds a fixed two-element `history` array,
newest predecessor first; unused entries are `null`. Each entry is a complete
canonical value with its request ID, digest, revision/incarnations, backend
properties and hooks. At applied `C` with history `[B, A]`, the saved local
transitions are `A -> B` and `B -> C`; they need not be consecutive consensus
revision numbers. The optional pending value is still retained separately.

FINISH writes the applied value, empty pending value and rotated history in the
same fsync/rename/directory-fsync operation. There is no window where the new
applied marker exists without its saved predecessors. A failed or interrupted
write retains the existing uncertainty/recovery requirement. BEGIN and unchanged
worker results must preserve history exactly; FINISH import must match the exact
old applied/pending/history values. Stale, cleared or fabricated history cannot
cross the local worker handoff. Disk recovery validates canonical digests, strict
keys/checksum, contiguous history slots, increasing revisions and identity-aware
backend lineage. The history is local application evidence, not peer consensus
or lease ownership evidence.

Why two predecessors: a fast member may finish `B -> C` while a slow member is
still collecting application confirmation for `A -> B`. On restart the saved
`A`, `B`, and `C` definitions permit cleanup and reconstruction of both exact
local completion scopes. Startup cleanup must finish every saved table before
reconstructing either scope. Stop hooks can repeat, so retained hook programs
must remain usable and idempotent while these records reference them. A failure
in a historical backend/hook blocks proof even if the current table was cleaned.

`lcs_config_startup_restore_completed()` then restores local APPLIED proof for
`B -> C` and the preceding completion record for `A -> B`, bound to the **new**
daemon instance. A slow member's exact older query can be answered. Other members
must discard old-instance replies and re-collect from admitted sessions; local
reconstruction does not restore a global certificate. A pending `C -> D` waits
until fresh full-member confirmation retires `B -> C`, then uses a new stop round.
No table remapping, lease admission or configuration vote is enabled by this step.

Record format 1 is still readable but has no recoverable completed predecessor
history. When its applied revision is nonzero, ordinary BEGIN/export preserves
format 1 until a real FINISH establishes a known predecessor; that FINISH writes
format 2 with one saved predecessor. Unknown older entries are never fabricated.
Startup cleanup may verify the available current backends, but cannot acknowledge
unknown completed scopes or bypass missing history to start another pending
scope. Missing-history recovery remains gated. Released legacy INI configurations have no distributed catalog
and retain their existing upgrade path.

The history is bounded: four possible full values (applied, pending and two
predecessors), each within the existing 512-KiB resource-document cap. The catalog
limit is 4 MiB plus 32 KiB to accommodate JSON string escaping and metadata.
The shared writer still limits parent pipe processing to 64 KiB per poll. No new
library, peer frame/layout/version or CLI protocol is needed for this disk format.

## Serving differing catch-up scopes during restart cleanup

The explicit `lcs_config_startup_bind_owner_history()` adapter lets a recovering
full member answer a different old-generation scope without replacing its pinned
local receipt. For example, at applied `C` with history `[B, A]`, its restored
receipt covers `B -> C`, while a peer that skipped `B` may ask about `A -> C`.
After physical cleanup of all three tables, the adapter recomputes that exact
`A -> C` scope from the saved values, including the dependency drain mask. Both
STOPPED and APPLIED queries can succeed for this exact current applied target.

STOPPED queries may also name the saved pending target, whose backends have not
been installed during recovery. APPLIED queries may not: pending, a higher
revision, and a matching resource name are not evidence of durable application.
Unknown old snapshots, conflicting digests, and incorrect drain masks provide no
additional proof. Existing pinned receipt/completion handling remains available
for its own exact scopes; this adapter neither replaces nor retires receipts.

The adapter borrows the startup runner until the owner network is stopped. All
backend/reactivation and recovery fences must remain held throughout that time.
Cancellation, catalog/history changes, poisoning, or leaving recovery invalidates
the provider and revokes active collection. Queued replies are evaluated again
before each send, including retries after backpressure. Full-member session,
instance, membership, attempt and deadline checks still apply on both ends.
Quorum-only nodes cannot register this provider or supply owner evidence.
Nonzero format-1 catalogs without predecessor history cannot register either.

This serves bounded known-history queries. Automatic recovery coordination and
missing predecessor transfer remain gated; no peer message layout, protocol
version, catalog format or CLI change is needed for this provider.

## Recovery handoff after catalog progress

A cleanup runner is valid for one exact applied/pending/history catalog. BEGIN
and FINISH therefore invalidate its provider. They do not let the daemon discard
its owner network and restart request numbering: delayed replies from the same
daemon instance must never match a newly reused attempt ID.

`lcs_config_startup_refresh_owner_history()` is the explicit handoff after catalog
progress while **all recovery/backend/reactivation fences remain held**. The
caller creates a new cleanup runner and retains the old borrowed runner. The new
runner must physically clean its current and retained historical tables, including
all hooks, and complete without error. Each runner can bind an owner provider only
once; a failed receipt cannot be cleared with its earlier cleanup result. Any old
cleanup worker must also be reaped before replacement.

The handoff preserves the daemon's request serial and peer-instance observations,
but drops old rounds, buffered replies and collected remote evidence. It rebuilds
exact local completed scopes from the new catalog and starts fresh all-full-member
confirmation on subsequent processing. A peer collection that received FAILED
while the old provider was invalid retains its receipt and requires an explicit
network retry after repair; later replies cannot silently revive the failed round.
Prior pinned/completed scopes must still
be answerable by the reconstructed receipt, preceding completion record, or exact
history provider; otherwise replacement fails without modifying the network.
Unknown/missing history, lost scope evidence, exhausted request IDs and a closing
network cannot be bypassed. The old runner may be closed only after a successful
handoff (or after stopping the owner network).

For example, node1 may have `C` applied with `D` pending while node2 has `B`
applied with `C` pending, although both have learned `D`. Node2 first retires its
`A -> B` receipt using node1's saved completion record. It then obtains exact
`B -> C` stop proof and finishes its existing pending `C`. After fresh cleanup and
the handoff, both members re-collect application confirmation for `B -> C`.
Only then do they proceed to `C -> D`; node2 persists that next pending intent
through the ordinary catalog writer. Catalog progress requires another cleanup
and proof handoff, never reuse of old-instance or old-attempt remote evidence.
The two-node regression uses separate durable stores and actual backend/hook
cleanup with dry-run backends, and verifies convergence through both targets.

This API does not perform BEGIN/FINISH writes, install/remap resources, clear
recovery fences, grant resource/configuration votes, or enable automatic startup.
The future distributed manager must drive those existing serialized operations
and this handoff in order. Unknown predecessor tables and incompatible target
histories remain blocked rather than inferred from revision numbers.

## Explicit recovery controller

`config_recovery.c` drives the recovery sequence once an existing durable barrier
has been recovered. It is an internal, explicitly started controller; the daemon
still refuses distributed startup until its concrete installation, generation and
lease-admission paths are connected. It does not activate dynamic commands.

Start it before ordinary backend/hook/probe/handoff workers are allowed to run,
or after cancelling/draining them under the existing global fences.
The controller takes the shared coordinator's application hold before starting
work and retains it even in `READY`. Every process call advances the storage
worker and owner exchange, using caller-supplied admitted peer instances and the
existing configured wire-6 transport. Cleanup and storage use their asynchronous
workers; no parent-side fsync, backend stop or hook wait is introduced.

The sequence is:

1. Preserve an existing durable pending target even when the learned checkpoint
   is newer. If there is no pending target and application is behind, persist
   BEGIN through the shared writer before cleanup captures its exact catalog.
2. Clean all applied and saved historical definitions, including hooks, then bind
   the first owner provider or hand off from the previous cleanup runner. Missing
   or unanswerable scope history fails closed.
3. Re-collect exact completed-scope application confirmation from every full
   member before starting the pending stop scope. Quorum-only nodes skip physical
   cleanup and owner evidence, but retain metadata/application obligations.
4. Obtain exact pending stop proof, then invoke the mandatory nonblocking table
   installation callback with an immutable target, plan and attempt ID. The
   callback restores/remaps definitions and incarnation-aware private promises
   while keeping backends, leases and votes fenced; it cannot admit resources.
   Already-current startup also requires this callback to restore its tables.
5. Recheck owner proof before installation and again before starting FINISH.
   Persist the exact target through the shared writer, then run fresh cleanup and
   renew proof with preserved request IDs. Finish the saved pending revision before
   staging the latest learned value. A newer checkpoint can wake a `READY`
   controller without releasing its hold or enabling votes.

`READY` means the latest learned catalog is installed, physically cleaned, and
its completed receipt has retired; **recovery and backend/reactivation fences
remain held**. The controller leaves `requires_recovery` set and never grants
resource/configuration votes or starts backends. Calling the concrete table and
projection adapter from daemon startup, and ordinary generation/restart/lease
admission remain separate integration work, as does missing-history transfer.

Backend/hook failures retain their runner and masks for an explicit repair/retry.
Failed or expired owner collections retain receipts and require a new collection
attempt. Installation retries preserve their immutable target; if proof was lost,
they wait for a fresh complete collection before invoking the callback again.
Neither an absent full member nor elapsed time supplies proof. Uncertain writes,
missing history, lost authority, or an unexpected catalog change are terminal:
stop owner replies, retain fences and require disk recovery. Callback and cleanup
failure details remain in the controller diagnostic.

Shutdown stops the owner provider, cancels both cleanup runners and the shared
writer, then continues polling until all child processes are reaped. Snapshots,
the writer lock and application hold cannot be disposed of early. Closing after
shutdown releases only that controller hold on an already-closing coordinator;
it never performs admission.

The regression harness uses two independent durable stores and actual shared
writer children with parent fsync forbidden. It automatically recovers differing
pending/applied revisions, injects cleanup/installation faults and session loss,
checks fresh proof on retry, and exercises checkpoint wake-up, quorum-only nodes,
failed fsync, writer/backend cancellation, offline members and unknown history.
No wire layout, disk format, public command or release version changes.

## Concrete recovery table installation

`config_recovery_live.c` supplies the controller's mandatory definition-table
installer. It remains an explicit adapter: the daemon's distributed-startup guard
is still present until scheduler and generation/restart admission are connected. It does not enable runtime mutation commands or reinterpret a partial
revision-zero initialization as activated state.

Call `lcs_config_recovery_live_prepare()` before listeners or ordinary work. It
requires valid recovered authority and a nonzero applied schema-2 catalog, no
peer/CLI/handshake sessions, no indexed operations or workers (including removed
resource slots), and no existing ownership epochs or private lease promises.
It decodes the exact last-applied definitions from durable state, never the INI
resource table or a newer pending target. The bootstrap must be frozen with no
legacy snapshot writer/queued refresh, matching state-directory/lock descriptors,
and an open projection directory/name. Static settings must still pass the
ordinary resource-only candidate boundary.

Preparation establishes global definition/remapping fences before cancellation,
publishes the applied generation with resource admission disabled, initializes
all resource/grant slots as unowned, and refreshes the ordinary restart waiting
floor. It does not clean a backend or authorize voting. Occupied slots and saved
promises cannot be silently erased to make preparation succeed. Preparation is a
one-time operation; errors after fencing leave fences held.

The install callback accepts only its controller's exact target and attempt in
INSTALL, with a valid snapshot-bound cleanup runner and global recovery fences.
Full members need the exact pending owner scope to be complete, or completed
receipt retirement for an already-current startup table. Quorum-only nodes retain
their metadata obligations without supplying backend-owner evidence. The adapter
validates complete old/target definitions, static settings and every incarnation
mapping before changing daemon tables. Pending workers/operations return pending;
conflicting attempts, scope evidence or mappings fail closed.

Generation cancellation runs against old indices before runtime tables are copied
and remapped. A matching name, birth revision and backend retains the observed
safety epoch, private grant (including an unexpired promise/deadline), failover
count, home policy and disabled policy at its new sorted index. Physical cleanup
means observed ownership is normalized to STOPPED/unowned with no active public
lease, queued worker or activation attempt. A new incarnation/backend gets fresh
unowned state and no old promise. Removed slots are cleared. Private promises are
never serialized as resource gossip or converted into observed ownership.

Closing generation sessions can invalidate a previously complete owner round.
The controller therefore rechecks proof before FINISH and requires an explicit
fresh collection before retrying installation/FINISH. The install callback is
idempotent for the exact attempt, full table and generation digest; a retry does
not close sessions again or reset preserved promises. It leaves recovery,
definition/remapping fences, the application hold and generation admission in
place. Ordinary state sync, restart waiting, quorum and final lease admission
remain mandatory separate steps.

The regression drives two recovery controllers with the concrete daemon-table
adapter on one full member. It inserts a resource before existing sorted indices,
changes another backend, preserves an unexpired private promise at the matching
resource's new index after cancellation callbacks, resets new/replaced slots, and
verifies fresh proof after generation-session invalidation. It also covers stale
generation retries, wrong maps, workers in removed slots, occupied pre-listener
state and a quorum-only concrete installation that performs no backend hooks.
The transport cancellation is simulated in this harness; live daemon/lab startup
is not claimed. No wire layout, public command or release version changes.

## Recovery projection publication barrier

The concrete recovery adapter now owns a `config_projection` provider borrowing
the daemon's frozen bootstrap and shared storage coordinator. Every successful
installation callback requires the **latest learned** value to have been durably
published to `resources.json`. This includes already-current startup tables,
quorum-only installations and idempotent retries after generation-session changes.
Publication is a derived-file obligation, not a decision or ownership certificate.

An existing pending application may be older than the learned checkpoint. The
adapter publishes the checkpoint, then installs its original immutable pending
target after the owner barrier. It never overwrites the projection with that
older target. If a checkpoint advances before a prior projection completion is
observed, the provider consumes that completion but publishes the new revision
before reporting success. Completion binds to the coordinator's revision/digest;
BEGIN/FINISH and projection jobs share the same single writer.

The callback validates its target, mapping, cleanup, owner proof and outstanding
workers before starting publication. No table changes, generation-session closure
or promise remapping occur until publication succeeds. Before each subsequent
callback the controller rechecks owner proof; an expired or disconnected collection
requires explicit repair/retry even if its projection write completed. A successful
projection never authorizes a lost owner barrier or bypasses catalog application.

Known destination errors (including an unsafe symlink) leave agreement/application
handles healthy and retain the pending target, old tables, promises and global
fences. Repair the destination and explicitly retry the blocked installation;
the provider publishes the current learned value again. A cancelled or broken
shared writer retains the normal uncertainty requirement and needs disk recovery.
The bootstrap, state lock and projection directory remain borrowed/immutable until
the shared writer has exited. Shutdown cancels and reaps that writer before the
outer owner may close its descriptors, including when the child was stopped
inside fsync. Parent event-loop code performs no projection fsync/rename.

Regressions cover a full-member symlink failure and repair before table changes,
a quorum-only checkpoint advancing before an earlier publication is observed,
exact file contents and mode, and cancellation of a stopped projection writer.
They verify the application/voting hold, recovery fences and unadmitted generation
remain intact. Projection errors cannot fall back to INI or turn file edits into
committed definitions. Daemon startup/scheduler and final generation/restart/lease
admission are still not enabled; there is no wire, catalog format or CLI change.

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
