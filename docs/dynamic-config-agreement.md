# Durable configuration agreement

Implementation status: the agreement core, storage worker, and snapshot-transfer
codecs, incarnation validation, local readiness barrier and application catalog
are implemented and unit-tested. Explicit peer readiness queries/replies are wired
into admitted wire-6 sessions. The network acceptor/learner is registered with
daemon peer sessions. Automatic
proposal coordination, activation, resource application and CLI mutation endpoints
remain unfinished. Separate
wire-6 bootstrap metadata now reports prepared definitions diagnostically; wire-5
compatibility and INI authority remain. This document
records the rules the remaining integration must preserve.

## Scope and identities

Agreement uses two-phase, per-revision majority consensus with a fixed voter set.
It assumes correct, non-Byzantine cluster members and durable storage. It does
not implement membership changes, cryptographic node authentication, or persistent
resource lease epochs. Quorum-only members store and vote on the same definitions
as full members but never execute resource backends.

Algorithm background: Leslie Lamport's
[Paxos Made Simple](https://lamport.azurewebsites.net/pubs/paxos-simple.pdf).

The membership identity is SHA-256 over canonical JSON containing the cluster
name, sorted node names and roles, and lease/renewal/peer-timeout settings. Node
addresses and other local daemon settings do not change this identity. Every
network operation must be checked against the configured membership identity.

A configuration value consists of a request identifier and a complete resource
JSON snapshot. Its identity is SHA-256 over their canonical JSON representation.
It includes schema, cluster name, revision, dependencies, interfaces, hooks,
groups, and every resource property. This deliberately differs from the legacy
peering fingerprint, which excludes some node-local fields. Hashing uses OpenSSL
libcrypto, not a newly implemented hash algorithm. Hashes detect identity changes
and accidental corruption; they do not authenticate senders.

Hash format 1 uses sorted object keys, compact JSON separators, ASCII-escaped
strings, integer-only numbers, and canonical resource/dependency ordering. Tests
include independently generated membership and value digest vectors. Changing
this representation requires an explicit format/protocol migration rather than
silently changing identities during a library or application upgrade.

Revision zero is the prepared bootstrap definition, with request ID `bootstrap`.
A subsequent slot identifies both its revision and the preceding value's digest.
The core accepts only the immediate next revision. Equal revision numbers with
different predecessors or contents are not interchangeable. Revision counters
must never be confused with resource epochs.

## Proposing and selecting a value

Any eligible upgraded member may coordinate a request. The transport must verify
that the coordinator's ballot carries that node's configured sorted index.
Ballots combine a counter with a five-bit node index. A proposer first persists
its own promise before sending phase-one requests, and chooses a counter above
the largest promise it has observed. Promises remain monotonic across slots and
daemon restarts; counter exhaustion rejects new proposals.

1. **Prepare:** request promises for one exact next revision, predecessor, and
   ballot. An acceptor persists a higher promise before replying. Replies include
   the value, if any, it previously accepted for that slot.
2. **Select:** collect distinct promises from a configured majority containing
   at least one full member. Select the value with the highest accepted ballot
   reported by that quorum. Only if none was reported may the coordinator use
   its client's requested value. Conflicting reports for the same accepted
   ballot invalidate the proposal.
3. **Accept:** seal the selected value so late replies cannot change it. Send
   that value at the prepared ballot. Each acceptor validates the complete
   canonical snapshot, persists it with its promise, and only then acknowledges.
   An acceptor rejects a different value at an already accepted equal ballot.
4. **Choose:** distinct acknowledgements for the exact selected value and ballot
   from a configured majority establish the decision. Duplicate, stale, or
   unrelated acknowledgements do not contribute votes.
5. **Learn:** persist the selected value as committed, record its predecessor,
   ballot, and acknowledgement mask, and clear the next-slot accepted value.
   Learning a decision does not itself start, stop, or move any resource.

A replacement coordinator follows the same prepare procedure. It cannot choose
a fresh value simply because the previous coordinator disappeared. Its client's
request may lose to a recovered value; the eventual CLI must report that outcome
accurately and must not report the client's requested edit as committed.

The quorum intersection argument is per slot: if value A was accepted by a
majority, every later prepare majority intersects that acceptance majority. The
intersecting acceptor reports its accepted value and cannot subsequently accept
a lower ballot. By induction over later ballots, the highest accepted value
reported by a valid prepare quorum must carry A forward. Equal ballots cannot
choose two values because acceptors reject equal-ballot changes. Thus correct
coordinators cannot choose different successors to the same predecessor. This
argument depends on not losing or rolling back persistent promises and accepted
values, and on authenticated message-to-node/ballot correlation in the transport.

The acknowledgement mask is an internal record of actual replies collected by
the correct coordinator. It is **not a signed quorum certificate**. The core
checks mask bounds and quorum composition but cannot prove that a peer really
received the votes it claims. The peer layer must enforce the non-Byzantine trust
boundary, exact request correlation, and existing source/secret checks. A lone
larger revision counter is never sufficient evidence of a configuration decision.

## Journal and crash boundaries

Each member's `agreement.json` record is bound to its local member name and
membership identity. It contains the committed snapshot and request ID,
predecessor digest, decision ballot/voters, highest promise, and optional
next-slot accepted ballot/value. A checksum covers the complete record. Parsing
rejects unknown fields, malformed values, duplicate keys, inconsistent state,
foreign membership/member identity, and checksum mismatches.

Whole-record replacement uses the existing private state store: sync the new
file, rename it, and sync the directory. In-memory state is published only after
that succeeds. Any failed persistence operation invalidates the handle for
further voting until its disk state has been reopened and recovered. This
includes a directory-sync failure after rename: the new record may already be
visible, so an error cannot mean rollback.

The asynchronous wrapper performs these operations in a child worker. The parent
drains a bounded result pipe without blocking and publishes the verified local
record only after a complete successful worker exit. It rejects records that
would roll back a promise, replace an equal committed revision, forget an accepted
value, or skip a committed predecessor. A distinct checkpoint job/import permits
validated revision gaps while preserving promises and local application state. A killed/incomplete worker requires disk
recovery before further configuration votes; an old worker result cannot repair
that uncertainty. Workers close inherited network/listener descriptors and die
with their parent.

Cancellation retains the worker PID until it is reaped. Journal recovery must
wait for that exit: sending SIGKILL alone does not prove that an in-flight disk
operation has stopped. This keeps late old writes from racing with a recovered
acceptor's next vote without blocking the lease-processing event loop.

Reopening a valid journal restores both promises and accepted values, including
an accepted majority whose coordinator crashed before announcing the decision.
A known initialized member must never recreate a missing journal from INI.
The core requires explicit bootstrap authorization to create its first record;
the daemon activation/recovery manager must decide when that is permitted.

## Frozen local initialization

`config_activation.c` provides a resumable local initialization primitive. Before
creating either durable journal or application catalog, it syncs `activation.json`
with the node/membership identity, complete canonical revision-zero baseline and
its digest, phase, and checksum. Record formats 1/2 use `initializing` and `prepared`
phases. Format 1 is retained for compatibility; the coordinated initialization
worker now uses format 2, requiring request history before preparation. These
describe local storage preparation, not a chosen activation decision.
Format 3 adds a prior `frozen` state: it preserves the exact effective definitions
and excludes legacy reloads, without authorizing agreement/application/request
history creation. This local responder primitive is not invoked automatically.
First creation requires explicit coordinated authorization; advisory peer metadata
is insufficient. Ordinary legacy startup does not create this record.

Recovery uses the frozen baseline even if INI resource definitions changed after
the intent was written. While `initializing`, it may create missing journal and
catalog records in that order, provided no promise, accepted value, committed
successor or pending application exists. Format 2 additionally creates/validates
empty request history before syncing `prepared`; format 1 retains its original
ordering. A catalog without a journal is rejected, preserving the requirement
to recover promises. Once `prepared`, missing required records are never
regenerated, even at
revision zero with no promise. A reservation may already exist before PREPARE.
Any uncertain write requires reopening disk state before proceeding.

Startup resumes an existing initialization before opening listeners. Healthy
format-3 FROZEN state can resume its coordination channel with resource voting
and placement fenced. Other unactivated initialization states still require
the remaining activation-aware peering/application integration. INI snapshot refresh refuses any existing intent.
The synchronous primitive runs only during startup or in the serialized storage
worker; it must not be called directly from the live event loop.

`lcs_config_agreement_worker_start_initialization()` uses the same worker as
agreement and application jobs. Before forking it copies and validates an immutable
revision-zero candidate entirely in memory. The child creates or resumes storage
and verifies that the format-2 intent, journal and applied catalog all describe
that exact pristine value, with no promise, accepted successor or pending
application, and that the required request journal is healthy and empty. The
worker cannot silently upgrade a format-1 intent or replace pre-PREPARE
reservations with an empty in-memory journal. Only an exact, complete local
acknowledgement and successful child exit publish the preallocated handles,
including the required request history. The parent never reopens files or syncs
storage to import that result. This compact same-host acknowledgement cannot import active history
and is never transmitted as agreement or readiness evidence.

Initialization waits for active/queued INI refreshes, then permanently locks further
refreshes on that bootstrap handle and withdraws legacy preparation. Cancellation,
child failure, uncertainty, or a changed target runtime poisons the local handles;
the child must be reaped before disk recovery. Even a visible prepared marker does
not make an unsuccessful worker return successful. A missing intent without
explicit authorization, a different frozen baseline, or existing promises/chosen
successors requires disk recovery rather than a pristine-result import.

The format-2 INITIALIZING marker is the first-initialization authorization for
missing request history; a pristine agreement alone is insufficient. No readiness
barrier or proposal may be admitted before preparation, so a resumed INITIALIZING
intent cannot have legally issued client reservations. Nonempty history in that
phase is rejected without resetting it. PREPARED requires existing valid history,
including any retained reservations; normal runtime recovery reads those facts
without writes. Initialization's compact acknowledgement accepts only empty
history and sends nonempty state through normal recovery instead.

The automatic initialization coordinator remains to be implemented. Its worker
is not yet invoked by ordinary daemon startup/peering. Preparing local files alone does not permit configuration voting
or switch resource authority.

### First-initialization authorization round

`config_initialization.c` collects explicit freeze confirmations in memory before
the first initialization worker may start. It requires a current upgrade-capable
instance for every configured member, including quorum-only members. The future
transport must supply zero for unknown, disconnected or unsupported members;
ordinary version/bootstrap advertisements must never become freeze receipts.
The caller must exclude local legacy reloads, including queued requests, before
beginning collection.

Each receipt binds the coordinator instance and increasing attempt, responder's
admitted instance, exact membership and canonical effective INI baseline. The
collector registers each query before queuing it and counts only replies from
queried members; a majority is insufficient. Busy members keep the round pending.
Conflicting current evidence or withdrawal of an acknowledged
freeze fails the round, even after every member has replied. Stale/unsolicited
receipts do not count. A fixed ten-second deadline is not extended by retries,
duplicates or completion. Polling and the final worker gate invalidate the round
if any admitted instance changes/disconnects or the local reload generation changes.

`lcs_config_agreement_worker_start_initialization_round()` also recomputes the
effective configuration binding before invoking the shared storage writer.
Incomplete, expired or mismatched authorization does not fork or create files.
An active/queued bootstrap refresh returns busy without consuming the round;
the caller must revalidate when retrying. Successful fork consumes the round and
keeps the existing permanent local reload lock through cancellation, uncertain
I/O and disk recovery. The older boolean initialization function remains a
low-level explicitly authorized storage primitive; automatic upgrade must use
the guarded round entry point instead.

These are same-process collection objects, not serialized proofs. The explicit
peer freeze exchange and fenced startup resumption are implemented below;
automatic initialization/activation coordination remains unfinished. The local durable responder preserves reload exclusion across
restart. Collector expiry alone must never authorize a responder to thaw.
A completed collection authorizes
only local storage preparation; it is neither the durable readiness barrier nor
a decision to switch to JSON authority.

### Durable reload freeze before initialization

`lcs_config_agreement_worker_start_freeze()` uses the same serialized writer as
agreement and application work. The future transport must validate an explicit
freeze query and exclude all active/queued legacy reload work before calling it.
It waits for bootstrap refresh workers, freezes an immutable copy of the exact
effective legacy resource definitions, and locks local reload immediately when
the storage child starts. Only successful child exit and publication permit a
freeze acknowledgement. A pending/failed/cancelled child cannot acknowledge or
release the freeze; uncertainty requires disk recovery after reaping.

The child writes format 3 of `activation.json` with phase `frozen` and the same
member, membership, baseline and checksum fields as earlier intent formats.
Agreement, application, request history and readiness barriers must all be absent;
existing files are never overwritten. Exact retries of an existing freeze do not
rewrite it, while a different baseline or advanced intent requires recovery.
No resource backend is stopped and no journal, promise or activation decision is
created by this operation.

Runtime recovery reads a FROZEN intent without writes and restores its stored
baseline despite later INI edits. It neither calls initialization resume nor
creates missing history. Contradictory history is rejected before listeners.
The daemon restores a healthy freeze for coordination as described below; it
never falls back to importing later INI resource definitions or enabling reload.

After a complete current all-member authorization round, the guarded initializer
may replace that exact frozen runtime. The child first syncs a format-2
INITIALIZING intent, then creates agreement/application/request history and
PREPARED in the existing order. A crash before that promotion leaves only FROZEN;
after promotion, ordinary initialization recovery may finish from the durable
authorization. The parent retains its old frozen handles until the child has
exited and the exact complete result has been validated. Failed publication or
I/O keeps reload excluded and requires recovery instead of thawing/reimporting.

### Explicit peer freeze exchange

Wire-6 frame types 47/48 carry a separate strict format-1 freeze query/reply body.
Queries bind both daemon instances, increasing attempt, membership and complete
baseline; their destination instance is nonzero and status is zero. Replies echo
the tuple with FROZEN, BUSY or MISMATCH. The established peer source, frame type,
kind, negotiated version, exact length and canonical digests are checked before
any storage operation. Only full members initiate queries, while both full and
quorum-only members persist and acknowledge freezes.

The responder requires every configured member to have an admitted upgrade-capable
session before starting storage. Unknown/offline/old quorum-only members therefore
block it too. Active or queued INI reloads and bootstrap refreshes return BUSY;
definitions are compared again before starting the worker. Existing durable
FROZEN state may be acknowledged without another write. It never counts as
post-initialization readiness or permission to vote on configuration.

`config_coordinator` owns the freeze job on its existing shared writer, excluding
all concurrent agreement/catalog/history jobs and retaining shutdown/reaping
rules. Before successful publication the responder can report only BUSY. If the
writer fails or is cancelled, reload remains excluded, recovery is required and
no FROZEN acknowledgement is sent. A requester disconnect or replacement cannot
cancel durable work or redirect its acknowledgement to another instance.

One bounded waiter per configured node retains the newest correlated query.
Exact retries do not extend its fixed ten-second deadline. Lower attempts are
ignored; reusing an attempt with different definitions is rejected. Terminal
replies survive output backpressure until the deadline, and obsolete or expired
waiters are dropped without thawing the durable freeze. Fresh attempts can query
already durable FROZEN state without rewriting it.

`config_peer_freeze_start()` explicitly starts collection only after the local
freeze is durable and all configured sessions support it. The scheduler sends
queries and passes correlated replies to the all-member collector, invalidating
it on session/generation changes or deadline expiry. Completion does not start
initialization or change resource authority. Ordinary startup does not invoke
this entry point; automatic initialization and upgrade activation remain unfinished.

### Fenced restart resumption

Before listeners or backend workers, `config_peer_resume_frozen()` accepts only a
healthy recovered format-3 runtime bound to the locked bootstrap store. It checks
pristine daemon tables, no pending reload/refresh, no open peer/client sessions,
and the exact canonical frozen baseline. Existing leases, epochs, workers or
journal handles are rejected instead of being reset through this startup entry.

The daemon then installs those frozen definitions and empty runtime indices under
resource voting/placement fences, locks bootstrap refresh, and restores the
shared coordinator in IDLE. Full members perform ordinary startup cleanup using
the frozen baseline; later valid INI backend/resource edits never select cleanup
targets. Quorum-only nodes perform no backend operations. The INI file still
passes normal parsing/static validation before recovery, so invalid syntax is
not ignored by this stage.

The restored node opens its coordination and read-only CLI channels, responds
to correlated freeze queries from its durable record, and can participate in an
explicit new freeze round. It neither grants resource leases nor hosts resources,
admits configuration votes, initializes journals or switches to JSON authority.
Existing resource-frame fences remain intact. Periodic unchanged advisory
metadata maintains the separate wire-6 coordination channel without resource
heartbeats or voting readiness; cold frozen nodes reject wire-5 HELLO admission.
Automatic progression from freeze to initialization/activation remains unfinished.

`config status` uses local diagnostic format 4 for phase `frozen`, with INI
authority, no admission, no revisions and no dynamic command capability. Read-only
export returns the retained baseline. Mutation/reload commands and SIGHUP cannot
thaw it. Repeated restart does not rewrite its record or create history. Existing
INITIALIZING/PREPARED runtimes without an activated application still fail closed
until their separate live resumption path is implemented.

After preparation, nonzero committed/accepted values must use the dynamic schema;
revision one must preserve the frozen definitions when assigning incarnations.
The generic agreement library alone also supports legacy-schema successors, so
activation recovery adds this stricter validation.

`agreement.json` checkpoints the latest committed value and pending next
slot. The checkpoint path below supports catch-up across multiple revisions;
request-status retention, backups, disaster recovery, and safe deletion of
historical status records still require integration design. Restoring stale disks or losing all agreement state is not
equivalent to an ordinary daemon restart and must not silently reset promises.

## Durable all-member activation barrier

The barrier core, serialized persistence worker, and explicit peer readiness
exchange are implemented. Automatic initialization and activation are not enabled. A readiness round begins only after local frozen initialization is
durable and pristine. It counts that local member and explicit durable-readiness
receipts for the same membership and full baseline digest from every configured
member, including quorum-only members. A majority is insufficient. Duplicate
matching receipts are idempotent; mismatched or out-of-membership receipts reject
an unfinished round. Advisory `CONFIG_BOOTSTRAP_STATUS` messages are not receipts.
The peer handler distinguishes those message types and binds readiness to the
admitted configured peer identity and negotiated wire-6 session.

`CONFIG_READINESS_QUERY` and `CONFIG_READINESS_REPLY` are separate wire-6 message
types with strict format-1 envelopes. Query handling performs no storage I/O or
initialization: a legacy INI-only daemon answers `NOT_READY`. A matching reply may
say `READY` only when the local durable initialized state remains pristine, INI
refresh is locked, no reload is in progress, and current definitions match the
frozen baseline. Source instances must match HELLO. A different definition is
reported as `MISMATCH`. Unknown or unsupported sessions cannot count.

The internal `config_peer_readiness_start()` entry point begins an explicitly
authorized collection after local initialization; ordinary startup never calls it.
Each attempt uses the coordinator daemon instance and a strictly increasing ID.
The collector records the currently queried peer instance, ignores stale or
unsolicited replies, counts only explicit `READY`, and waits for `NOT_READY` peers.
Retries and reconnects do not extend its ten-second deadline; disconnected peers
also reach timeout. A reconnecting instance must be queried before its reply can
count. Confirmed durable readiness survives later disconnection, and a complete
sealed round does not expire. The scheduler sends only on admitted wire-6 sessions.

Collection alone never writes `barrier.json`, grants configuration votes, or
switches authority. After an explicitly started peer round completes, the
scheduler now submits that sealed round to the shared barrier storage worker.
The local coordinator becomes eligible only after successful durable publication;
a complete in-memory mask or visible-but-unacknowledged file is insufficient. Neither
peer metadata nor a readiness query triggers freezing, so mixed-version and
INI-only clusters retain their current startup/reload behavior. Once initialization
locks definitions, CLI/SIGHUP resource reload and peer reload transitions are
rejected or ignored; the frozen baseline cannot be replaced by a legacy reload.

Each member confirms an irreversible frozen baseline, so a valid receipt need
not expire merely because that peer disconnects. This relies on correct members
retaining durable state; disk replacement is still explicit disaster recovery.
Once all readiness has been collected, a sealed round authorizes a local
`barrier.json` record. Record format 1 contains exactly the local member,
membership digest, baseline digest, complete configured-voter mask, record version
and checksum. It uses the same canonical checksum representation, atomic replace,
file/directory sync and node binding as the other records. The mask is evidence
collected by correct non-Byzantine participants, not a signed certificate or
cryptographic authentication. It cannot authorize membership changes.

First creation requires pristine initialization with no promise, accepted value,
committed successor or pending application. Missing readiness after voting is
not silently regenerated from INI or fresh advertisements. Existing valid proof
is read without rewriting it, including after later configuration revisions.
The shared storage worker persists proof outside the lease event loop, reopens
and checks the child's disk state before writing, and publishes the parent handle
only after a complete acknowledgement and successful child exit. Cancellation,
uncertain sync or changed parent state poisons local handles until reaping and disk
recovery. A visible record alone cannot make an unsuccessful worker return success.

`config_coordinator.c` owns the handoff and one serialized configuration worker.
Its phases distinguish collection, persistence, durable readiness, local vote
writes, cancellation, stopped state and terminal failure. It binds a persistence
result to the originating requester instance, attempt and exact sealed baseline.
Changing that context while a child is pending cancels the child and requires
recovery; its late result cannot authorize a replacement attempt. The parent does
no store reads or filesystem syncs during this handoff.

`lcs_config_coordinator_start_vote()` admits explicit local PREPARE/ACCEPT jobs only
after the durable readiness prerequisite succeeds and local application matches
learned configuration. These jobs share the barrier writer, so an outstanding
write cannot race another vote. ACCEPT additionally requires a schema-2 successor;
revision-one activation must preserve the frozen definitions. Successful worker
completion reports the actual agreed/rejected/stale result separately from
uncertain persistence. Acceptance does not learn a majority decision, switch
authority or apply resources. Wire-6 acceptor framing now verifies proposer
identity before invoking these entry points. The internal network proposer uses
these gates; live CLI mutation commands are not enabled.

After `runtime_open`, the coordinator can adopt an existing valid barrier entirely
in memory, including when application is behind so checkpoint recovery can run. It does not need new peer observations
or rewrite proof. A pending application or poisoned handle still prevents voting.
The current daemon continues refusing distributed startup until activation-aware
resource application/recovery is implemented, so this local recovery entry point
does not enable automatic migration yet.

Shutdown stops collection and cancels any configuration writer while continuing
to poll child completion alongside resource shutdown. Runtime/store handles are
released only after reaping. If a child has not exited by the shutdown deadline,
the process retains those handles until exit; an inherited writer lock remains
held until the child itself exits. No recovery or replacement writer may start
under uncertain old I/O. Cancellation before a writer starts does not poison
otherwise healthy local journals; uncertain persistence requires disk recovery.

`lcs_config_barrier_can_vote()` is the configuration-vote prerequisite: a complete
local durable barrier must match the frozen baseline and membership, every local
handle must be healthy, and applied configuration must equal learned configuration
with no pending application. Learning revision one therefore blocks voting on
revision two until application catches up. This predicate does not switch JSON
authority, start backends, replace ordinary lease/restart recovery gates, or yet
admit peer configuration-transaction messages. The local coordinator vote path
uses it before starting its storage job. Initial authority still needs a majority-chosen schema-2
activation value and safe local application. Recovered durable readiness does not
require all members to remain online for subsequent majority-based edits.

Startup checks for orphan, premature, malformed, foreign and incomplete barrier
records before INI refresh or listeners. An existing valid barrier is retained
alongside recovered agreement/application state; the current daemon still refuses
to run distributed state until peering and application integration are complete.
A missing barrier leaves the configuration-vote predicate false. Existing
initialization or readiness markers also prevent direct/background INI refresh.

## Snapshot transfer

The implemented transport-independent BEGIN/CHUNK codecs use a bounded metadata
envelope containing transfer ID, operation kind, membership identity, revision,
predecessor/value digests, request ID, ballots, voter mask, and total length.
Chunks carry an ID and offset and contain at most 16 KiB. A receiver holds at
most one document of up to 512 KiB per authenticated peer, with a fixed ten-second
deadline. Identical retries do not extend that deadline or reset progress.

The receiver rejects gaps, conflicting duplicates, wrong IDs, invalid fields,
oversized lengths, noncanonical fixed-string padding, and foreign membership.
Completion validates the entire resource document and checks the declared
revision and canonical value digest before returning a candidate. Receiving
bytes never produces an agreement vote or changes runtime resources.

These codecs are now wrapped by wire-6 acceptor transaction messages after a
valid HELLO. The internal proposer consumes matching PROMISE snapshots and the
checkpoint receiver consumes requested SYNC snapshots. Keep the released wire-5
HELLO payload unchanged; negotiate support and exchange dynamic configuration
status after establishment rather than appending
fields that old strict-length decoders reject.

Application ordering, stable identities, crash recovery and client outcomes are
specified in [the application contract](dynamic-config-application.md). The
remaining activation/application requirements follow the implemented network
endpoint description below.

## Network acceptor and majority decision handling

`config_transaction.c` registers separate wire-6 control and bounded snapshot
messages. Every frame carries source and target daemon instances, checked against
the admitted configured peer. PREPARE, ACCEPT and LEARN must use a ballot whose
index identifies that source node. Slot/value/membership checks remain required;
a successful HELLO alone never grants configuration readiness. INI-only nodes
answer not-ready without reading or initializing durable configuration files.

One durable operation is in flight through the shared coordinator writer, with a
bounded per-peer response slot. PREPARE returns a PROMISE only after its journal
write completes and parent memory imports the durable result. If a prior value is
accepted, that reply identifies its ballot and digest and streams the complete
immutable snapshot; the proposer validates the entire snapshot and exact
request correlation before counting it. ACCEPT first persists or checks the selected
request identity through the coordinator, then receives a positive ACK only after
its value is durably accepted. A conflict at the same ballot rejects without
replacing the accepted value. Busy/backpressured requests may require retry.

LEARN requires a configured voting majority including a full member, valid
membership/parent identity and an immediate schema-2 successor. The voter mask
uses the existing non-Byzantine evidence model; it is not a signed certificate.
Learning writes agreement only: it neither updates applied state nor touches
backends or resource authority. Once learned configuration is ahead of local
application, new configuration votes are blocked. An exact current decision may
still be acknowledged idempotently while application is behind, after verifying
the durable barrier and majority evidence. The separate checkpoint path below
permits multi-revision catch-up.

Snapshot BEGIN is admitted before allocation, with at most one 512-KiB incoming
and one outgoing document per configured peer, chunks up to 16 KiB, and fixed
ten-second input/output deadlines. Identical retries never extend those deadlines.
The scheduler queues at most nine output frames per call (four acceptor replies,
four proposer requests and one checkpoint query), retaining immutable accepted values while
backpressured. Expiration discards transport buffers, never
a journal promise, accepted value or decision. A transfer alone never votes.

Disconnect clears session buffers and suppresses the pending response, but does
not cancel an already submitted durable write. Completion is correlated to the
specific coordinator operation; an unexpected completion or uncertain persistence
cannot produce a positive vote. Outcome-unknown is reported separately from
rejection and must not be interpreted as an abort. Recovery retains any visible
promise/decision before another attempt. Shutdown keeps the previous coordinator
reaping/lock rules. No final client outcome or client exactly-once semantics are
introduced by transport request IDs.

The internal network proposer now drives an explicitly submitted snapshot through
local durable request reservation, local durable PREPARE, distinct remote promises,
local durable ACCEPT, remote acceptance quorum, and local durable LEARN. It sends
no proposal frames before the original requested identity and its own promise
are durable. A healthy opened request journal is required; missing history is
never initialized by submission. Known request IDs, including IDs already present
in learned/accepted values, are refused even with a new transport ID. Each reply must match the exact request ID, revision, ballot,
membership, parent and admitted daemon instance. A promise reporting an accepted
value counts only after its complete snapshot validates; the highest accepted
value in the promise quorum takes precedence over the newly requested value.
Duplicate replies cannot add votes. ACCEPT is sent only to members of that
complete promise quorum; decision notification is sent only to acknowledged
acceptors. Other members can pull the latest decision through checkpoint synchronization.

One proposal and the acceptor share the existing storage writer. The proposal has
a fixed ten-second deadline; backpressure and duplicate frames do not extend it.
Disconnect invalidates that peer's pending response stream without cancelling a
durable write. Failure or expiry before local durable learning reports unknown,
even if a majority may already have accepted the value. A new attempt requires a
strictly newer process-local transport ID, a fresh persistent request ID and a
higher ballot. Reusing a reserved ID is a status/recovery case, not authorization
to submit again. There is no automatic retry; the read-only local `config request-status` command
reports journal facts, not final client success. Once local learning succeeds,
notification timeout cannot erase the known decision. Completion identifies the
chosen value, which may belong to an earlier recovered request, and never means
that resources have been applied or all members have received the decision.

Unsolicited control replies and PROMISE snapshots do not confer votes.
The three-node transport harness drives the actual
network proposer with independent durable stores, including remote accepted-value
recovery, duplicate replies, mismatched IDs, deadline expiry and a higher-ballot
retry. Automatic initialization/activation and dynamic commands remain disabled
until coordinated initialization, request lifecycle and public mutation handling are complete.

## Committed checkpoint synchronization

The existing wire-6 control frame adds `SYNC_QUERY` (kind 5) and `SYNC_STATUS`
(kind 6); no new frame IDs or HELLO/lease layout changes are needed. A query binds
a strictly increasing local ID, both daemon instances, static membership,
requester's learned revision/digest and frozen baseline digest. A server with a
different baseline rejects the request. A prepared node with a valid durable
barrier can serve its latest already committed decision, including when its own
application is behind. INI-only nodes return not-ready without allocating a
snapshot or creating history. No live quorum is required just to relay an already
chosen decision; the original configured majority evidence remains mandatory.

If the server is ahead, STATUS identifies the immutable latest revision, ballot,
parent and value digest, then a bounded SYNC snapshot carries the complete value,
request identity and original majority mask. The ballot identifies the original
proposer; the relaying peer may be a different node. Unsolicited/stale responses
never initialize state or allocate a receiver. Membership, baseline, request and
instance admission precede transfer; ballot, majority and complete-value checks
precede the storage job. A mask is the existing non-Byzantine decision evidence,
not a cryptographic certificate or a substitute for configured peer admission.

A checkpoint must advance or match local learned state. It cannot replace an
existing revision with a different value/parent. Immediate successors retain the
normal parent/incarnation validation. For a revision gap, any resource birth at
or before the local learned revision must name the same existing incarnation and
backend; absent/replaced incarnations must have a later birth. These checks
prevent an offline node from reusing deleted/recreated resource state. An old
accepted next-slot value may be discarded only as covered by a later committed
checkpoint; an equal/newer-ballot conflict in that same slot still rejects.
Existing promises never decrease, even when greater than the checkpoint ballot.

The explicit checkpoint action uses the same serialized background writer.
Normal worker imports still reject revision gaps; only checkpoint imports admit
validated jumps. Local applied and pending snapshots are retained unchanged,
including old cleanup hooks/backend definitions. Sync completion means durable
learned metadata, not backend application or voting eligibility. The node cannot
vote until application catches up with no pending cleanup. Timeout, disconnect,
or uncertain I/O cannot cancel or roll back an already submitted durable write;
recovery must inspect the journal after an unknown outcome. Matching/lower server
revisions are read-only. IDs and ten-second deadlines are never reused/extended.

The scheduler polls admitted wire-6 peers in rotation after a durable barrier is
available, with one pull in flight and at least a second between starts. This does
not authorize automatic initialization, switch resource authority, or bypass the
existing refusal to start a daemon with distributed history. Startup/resource
reconciliation and automatic activation remain unfinished. The transport harness
covers multi-revision delete/recreate catch-up, a relayed ballot, quorum-only
receivers, retained pending cleanup and higher promises, recovery from disk,
baseline mismatch, fixed deadlines, stale instances and disconnect during a write.

The internal [reconciliation sequencer](dynamic-config-application.md#internal-reconciliation-sequencer)
now shares this writer for catalog BEGIN/FINISH, with a separate completion counter
and an ownership hold through backend/generation admission. A learned checkpoint
can advance while cleanup is in progress without replacing its pending target.
The sequencer and explicit backend barriers remain disconnected from live daemon
resource application until safe generation/owner adapters are implemented.

## Local durable request-fact journal core

`config_requests.c` provides an explicit storage primitive for durable local request facts.
The daemon recovers an existing journal before opening listeners; it does not
automatically initialize it or enable mutation commands. The read-only local
`config request-status ID` query exposes published reservation/commit facts, with
explicit unknown and unavailable/recovery-required history states. It performs no
disk I/O or proposal retry and does not claim application or client success.
The shared nonblocking storage worker now supports reservation and learned-decision observation for an explicitly opened
history. These jobs exclude agreement/application writers on the same worker and
publish monotonic history only after a complete result and successful child exit.
The parent performs no journal writes. Logical conflicts/capacity responses leave
handles usable; uncertain writes, cancellation or incomplete handoff poison both
history and agreement until the child is reaped and disk state is recovered.

Startup recovery binds existing history to the frozen static base, rejects
unsafe/corrupt/foreign files and orphan history without agreement/application
state, and checks facts against the current learned and accepted values. A
journal ahead of the learned state, a mismatched current decision, or an accepted
value reusing an immutable identity fails startup without rewriting the history.
Older entries behind checkpoint gaps cannot be verified from the current value
alone; recovery retains them without inventing missed outcomes. A crash after
learning but before history observation does not silently upgrade a reserved bit.

Older catalogs without `requests.json` remain recoverable, with history unavailable.
Recovery never creates an empty file or authorizes identifier reuse from absence.
The shared storage worker now also offers explicit request-history open/recovery.
It reopens the disk agreement in its child and compares the complete committed
value, promises, accepted value and learned evidence with the parent's immutable
snapshot. Existing history passes the same checks as startup recovery and is not
rewritten. Publication waits for a complete handoff and successful child exit;
failed/cancelled jobs poison both the agreement and destination until reaping and
recovery. The destination must be fresh and cannot replace an existing handle.

Explicit first-initialization authorization can create a missing journal only
at pristine revision zero without promises, acceptances or learned successors.
It never resets corrupt history or initializes an advanced missing history.
Pristine agreement alone cannot prove no earlier request IDs were reserved before
PREPARE, so callers must establish coordinated first-initialization authorization;
absence is never authorization for automatic repair after history loss.

The transaction coordinator now schedules explicit OPEN (existing history only),
RESERVE and OBSERVE jobs on its single storage worker. Its request phase blocks
other configuration writers and voting until completion. Request completion has
its own counter/result: it does not advance vote, application or projection
counters, and records neither admission nor a final client outcome. Logical
EXISTS/CONFLICT/CAPACITY results keep the coordinator usable; uncertain persistence
or cancellation poisons the handles and requires child reaping and disk recovery.

New request IDs require the normal voting prerequisite and a valid dynamic
successor, including definition-preserving activation. Known-ID checks can return
EXISTS/CONFLICT while application is behind, but cannot start another proposal.
Observation of the current learned decision is also allowed while application
lags or a reconciler holds the application owner. Neither operation releases the
owner's fence, changes the applied revision or runs a backend. OPEN deliberately
rejects initialization authorization at the coordinator boundary.

Local proposer submission now reserves the original requested ID, expected base
and complete canonical value through the coordinator before PREPARE. The private
RESERVING phase uses the request-completion counter; subsequent storage phases
use the vote counter. Failure, capacity exhaustion, mismatched completion or
reservation timeout cannot start PREPARE or send proposal frames. A timed-out
writer still runs to completion and retains its durable identity; shutdown keeps
its handles until cancellation is reaped and restart reloads the journal.

The coordinator's ACCEPT path now requires healthy existing request history.
After read-only slot/ballot/canonical-successor checks, it owns a copy of the
selected value across an ACCEPT_RESERVING phase. The shared worker persists or
checks that value's exact ID/base/fingerprint before a second worker persists
acceptance. No positive ACK or vote completion is produced by reservation alone.
Exact known identities are allowed for protocol retries and highest-accepted
recovery; changing their fingerprint or exhausting space for a new identity
rejects the vote without replacing an accepted value. Stale/rejected ballots do
not consume history entries. Request checks and acceptance retain separate
completion counters and exclude every other configuration writer between phases.

If reservation persistence is uncertain, both handles require recovery. If the
later acceptance is uncertain, the durable request fact remains, but the agreement
is fenced for disk recovery. Stop/cancellation clears the owned value and retains
child/storage handles until reaping. Restart can resume an exact protocol ACCEPT
without duplicating or rewriting its identity. A matching client proposal remains
a separate status/recovery case and cannot be resubmitted by changing transport ID.

The proposer reservation fingerprints the originally requested value. Recovering
a higher accepted value through promises adds/checks its separate selected-value
identity without overwriting original intent or claiming that it committed.

Successful LEARN and checkpoint persistence now schedule observation of the exact
current learned request when history exists. The coordinator publishes consensus
completion first, then holds an OBSERVING phase on the same writer until history
records/retains that decision. This includes local proposer learning, remote LEARN,
duplicate learned decisions and checkpoint catch-up. Missing legacy history stays
unavailable and is not initialized by a learned message. A checkpoint gap records
only its latest proven outcome, never invented results for skipped revisions.

Consensus replies and proposer/checkpoint completion mean that the decision was
learned durably; they do not wait for or imply history durability, application or
client success. If observation fails or is cancelled, a known decision cannot
become aborted/unknown merely because bookkeeping failed. The coordinator fences
further configuration work and requires recovery; the durable agreement remains
the source of the learned decision. Conflicting identities or insufficient space
for a previously unseen learned request also fence progress without rolling the
decision back. An exact duplicate LEARN can finish missing observation after a
healthy disk recovery. Existing history alone is not proof of lifecycle readiness;
public definition commands additionally require activated JSON authority and local
admission, as described in [runtime commands](resource-json.md#runtime-definition-commands).

Automatic first-initialization authorization, lost-history/checkpoint recovery,
distributed client outcome recovery, retention and all-member admission
acknowledgements remain unfinished. Origin submission/wait handling uses durable
local admission receipts and conservatively retains unknown results elsewhere.
Callers must keep both handles unchanged until completion, use the same worker
for every store writer, and explicitly authorize first initialization. An inherited
directory-lock descriptor alone cannot serialize multiple writer children.

The member/membership-bound `requests.json` record contains a checksummed,
append-only list of observed request identities. Each entry binds the request ID,
expected base revision and complete base-value digest to the complete requested
value digest. That digest comes from the canonical value constructor, including
request ID, resource revision, incarnations and complete definitions. Reserving
a new identity requires the exact current agreement base; exact retries retain
their original base even after learning advances. A different base or content
for an observed ID conflicts and cannot overwrite its record. Reservation is
not proof of successor eligibility or permission to submit another proposal.

Only observation of the current exact locally learned majority decision can
advance `committed` from false to true. An accepted value or promise does not
do so. This bit records a decision fact, not resource admission, successful
client completion or cluster-wide application. There are no fabricated abort or
rejection outcomes: an absent entry, a reserved-only entry or an unobserved
intermediate decision remains uncertain. Checkpoint gaps cannot reconstruct
missed request outcomes from a higher revision number alone.

After actual local application admission and its final handoff, the manager now
persists an `applied` receipt through the same writer. Its trusted same-process
receipt binds the exact attempt/revision/digest; the storage job also requires a
healthy caught-up applied catalog without pending work and the exact learned
majority decision. FINISH, timers, higher revisions and already-current shortcuts
cannot authorize a receipt. Missing history remains missing. Interrupted writes
retain ordinary poisoning/reaping rules and never rewrite a known decision.

Request record format 2 adds a monotonic `applied` boolean (which requires
`committed`). Format 1 remains readable with unknown admission, without read-time
rewriting or inference from the catalog. Imports cannot regress either bit.
Recovery rejects receipts ahead of or conflicting with the durable applied
catalog. Historical local admission is distinct from current resource health,
all-peer acknowledgement and the unfinished full client wait/response lifecycle.

The core currently holds 128 identities with a 128-KiB record bound. It never
evicts an observed ID: capacity refuses new entries while exact retries remain
readable/idempotent. This is an internal storage bound, not a limit imposed on
current legacy INI operation. Explicit activated transactions use this bound. Before exposing
mutation commands, retention/compaction and missing-history behavior must be
integrated explicitly rather than silently forgetting identifiers.

Explicit initialization authorization creates only a missing record; corrupt or
foreign existing state is never reset. A newly initialized empty record says
nothing about earlier requests. Atomic replacement uses private mode 0600,
file sync, rename and directory sync. Write uncertainty poisons the handle until
disk reopen. Worker export/import is same-host state transfer only; import can
append entries or advance committed/applied bits, but cannot truncate, reorder, change
identities or regress confirmed decisions. Invalid or regressive imports poison the handle
without replacing its last in-memory records.

Regressions exercise process exits before/after rename, independent child writes
and parent import, failure at both sync boundaries, exact retries after learning,
ID/base/content conflicts, accepted-only uncertainty, foreign/malformed records,
symlink refusal, capacity and monotonic replay. No peer/CLI protocol, existing
agreement/application record or release version changes accompany this core.

## Remaining daemon activation and application rules

- Confirm support, durable readiness, matching full definitions, and compatible
  static membership from **every configured node**, including quorum-only nodes,
  before the first activation. Offline/unknown members delay activation.
- Serialize that barrier against INI reload and all bootstrap/storage workers.
  Freeze the prepared predecessor while activation is in progress.
- Automatically agree on an activation value that changes authority without
  changing resource definitions. This application-specific rule must be enforced
  by the activation manager; the generic agreement core does not enforce it.
- Persist authority and project the committed snapshot to `resources.json`.
  After activation, the journal/decision history is authoritative; JSON is a
  recoverable projection. Never refresh active state from stale INI sections.
- Persist a last-applied snapshot and a cleanup catalog before touching backends.
  A committed snapshot alone cannot identify a deleted resource still running
  after a crash. Keep old/pending backend definitions until cleanup and the
  applied marker are durable; missing projection/catalog state requires safe
  recovery or fencing, not blind regeneration of only the newest definitions.
- Serialize local application across revisions and require the applied revision
  to match before admitting normal resource votes. Do not lose intermediate
  cleanup information when catch-up spans several revisions.
- Admit returning or stale nodes only for configuration synchronization until
  their definitions, cleanup, and recovery are complete. Reject old binaries
  after activation instead of letting a reconnect lower the active feature floor.
- Apply changed/deleted backends through safe drain/dependency ordering and
  lease rules. Preserve runtime identity by resource name for unchanged resources;
  do not preserve an old incarnation's grants after deletion/recreation.
  Schema-2 snapshots now persist `(name, birth_revision)` incarnations and the
  core validates their successor rules. The daemon must consume these identities
  when reconciling resources and processing peer messages.
- Bind resource messages to the applied configuration generation, and flush old
  sessions before index remapping. Delayed old-schema resource IDs must never be
  interpreted against a new sorted resource table.
- Distinguish a chosen configuration decision from completed local resource
  application. Define client committed/pending/failed/unknown responses and
  durable request status before exposing mutation endpoints.
- After successful activation, continue with normal majority requirements;
  later offline nodes must not permanently disable dynamic edits.
