# Durable configuration agreement

Implementation status: the agreement core, storage worker, and snapshot-transfer
codecs, incarnation validation, local readiness barrier and application catalog
are implemented and unit-tested. Explicit peer readiness queries/replies are wired
into admitted wire-6 sessions. Durable transactions are not connected to daemon peer sessions,
automatic activation, resource application, or CLI mutation endpoints. Separate
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
value, or skip a committed predecessor. A killed/incomplete worker requires disk
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
its digest, phase, and checksum. Record format 1 uses `initializing` and `prepared`
phases. These describe local storage preparation, not a chosen activation decision.
First creation requires explicit coordinated authorization; advisory peer metadata
is insufficient. Ordinary legacy startup does not create this record.

Recovery uses the frozen baseline even if INI resource definitions changed after
the intent was written. While `initializing`, it may create missing journal and
catalog records in that order, provided no promise, accepted value, committed
successor or pending application exists. It syncs `prepared` only after both records
are durable. A catalog without a journal is rejected, preserving the requirement
to recover promises. Once `prepared`, missing records are never regenerated.
Any uncertain write requires reopening disk state before proceeding.

Startup resumes an existing initialization before opening listeners, but still
refuses to run distributed state until the remaining activation-aware peering and
application integration exists. INI snapshot refresh refuses any existing intent.
The synchronous primitive runs only during startup or in the serialized storage
worker; it must not be called directly from the live event loop.

`lcs_config_agreement_worker_start_initialization()` uses the same worker as
agreement and application jobs. Before forking it copies and validates an immutable
revision-zero candidate entirely in memory. The child creates or resumes storage
and verifies that the intent, journal and applied catalog all describe that exact
pristine value, with no promise, accepted successor or pending application. Only
an exact, complete local acknowledgement and successful child exit publish the
preallocated handles. The parent never reopens files or syncs storage to import
that result. This compact same-host acknowledgement cannot import active history
and is never transmitted as agreement or readiness evidence.

Initialization waits for active/queued INI refreshes, then permanently locks further
refreshes on that bootstrap handle and withdraws legacy preparation. Cancellation,
child failure, uncertainty, or a changed target runtime poisons the local handles;
the child must be reaped before disk recovery. Even a visible prepared marker does
not make an unsuccessful worker return successful. A missing intent without
explicit authorization, a different frozen baseline, or existing promises/chosen
successors requires disk recovery rather than a pristine-result import.

The automatic initialization coordinator remains to be implemented. The initialization worker is not yet invoked by ordinary daemon
startup/peering. Preparing local files alone does not permit configuration voting
or switch resource authority.

After preparation, nonzero committed/accepted values must use the dynamic schema;
revision one must preserve the frozen definitions when assigning incarnations.
The generic agreement library alone also supports legacy-schema successors, so
activation recovery adds this stricter validation.

`agreement.json` checkpoints the latest committed value and pending next
slot. Catch-up across multiple revisions, request-status retention, backups,
disaster recovery, and safe deletion of historical status records still require
integration design. Restoring stale disks or losing all agreement state is not
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
authority or apply resources. Peer PREPARE/ACCEPT transaction framing and proposer
identity admission are not implemented yet; these entry points are not live
mutation commands.

After `runtime_open` and application recovery, the coordinator can adopt an
existing valid barrier entirely in memory. It does not need new peer observations
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

These codecs are not currently registered as peer message types. Integration
must negotiate a new feature version and send new messages only after a valid
HELLO. Keep the released wire-5 HELLO payload unchanged; negotiate support and
exchange dynamic configuration status after establishment rather than appending
fields that old strict-length decoders reject.

Application ordering, stable identities, crash recovery and client outcomes are
specified in [the application contract](dynamic-config-application.md). The
following integration requirements remain unimplemented.

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
