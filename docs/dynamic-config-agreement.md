# Durable configuration agreement

Implementation status: the agreement core, storage worker, and snapshot-transfer
codecs, incarnation validation and local application catalog are implemented
and unit-tested. They are not connected to daemon peer
sessions, automatic activation, resource application, or CLI mutation endpoints.
The running daemon still uses wire version 5 and INI authority. This document
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

The current record checkpoints the latest committed value and pending next
slot. Catch-up across multiple revisions, request-status retention, backups,
disaster recovery, and safe deletion of historical status records still require
integration design. Restoring stale disks or losing all agreement state is not
equivalent to an ordinary daemon restart and must not silently reset promises.

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
