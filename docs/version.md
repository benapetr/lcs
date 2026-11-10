# Version update locations

Update these files when bumping the LCS release version:

- `src/common.h`
- `packaging/lcs.8`
- `packaging/lcsd.8`

Release-version checks in tests should read `LCS_VERSION` from `src/common.h`
instead of hard-coding an application version. Protocol release labels describe
the release that introduced a wire version: wire version 5 remains mapped to
`1.1.0` when the application release changes to `1.1.1`.

## Peer protocol compatibility

`LCS_PEER_PROTO_MIN_VERSION` and `LCS_PEER_PROTO_VERSION` in
`src/protocol.h` define the supported wire-version range. The HELLO exchange
negotiates the highest version supported by both peers. The cluster effective
version is the oldest currently confirmed version across all configured
members; a disconnected or not-yet-negotiated member contributes the minimum
supported version.

Future peer protocol changes should follow these rules:

- Keep the released HELLO prefix decodable across the supported range.
- Prefer new message types over changing the layout of an existing message.
- Assign each new feature a minimum protocol version and reject its messages
  on connections negotiated below that version.
- Gate cluster-wide state transitions on the cluster effective version, not
  only the coordinator's local version.
- Raise the minimum supported version only in a release that intentionally
  ends rolling-upgrade compatibility with older releases.

Version 5 establishes this negotiation contract and includes fixed
online-participant sets for resource reload. Reload checks the negotiated
version of every transaction participant rather than the global effective
version, because an offline nonparticipant conservatively contributes the
minimum version and must not block an otherwise compatible online quorum. It
intentionally does not decode the earlier version-3 HELLO because no release
was made between those formats.

## Unreleased wire version 6

Wire 6 adds the separate `CONFIG_BOOTSTRAP_STATUS` message (type 30), sent only
on established sessions negotiated at version 6 or above. Minimum peering stays
at 5; the complete released HELLO payload layout and all existing lease/reload
messages are unchanged. A wire-5 member never receives the new message.

The metadata reports a session-local serial, durable shadow preparation flag,
canonical membership digest, and complete revision-zero resource value digest.
It is advisory: it does not promise a ballot, acknowledge durable activation
readiness, establish authority, or enable dynamic commands. Initial activation
still requires explicit durable readiness from every configured member, persisted
barrier proof, a chosen activation value and safe local application.

Wire 6 also defines `CONFIG_READINESS_QUERY` (31) and
`CONFIG_READINESS_REPLY` (32), with a separate format-1 body. These are ordinary
established-session messages, not HELLO additions or RPC replies; each frame uses
a fresh sequence and the body correlates requester instance, monotonic attempt,
and responder instance. Both bodies contain a kind, membership and baseline
digests; a query has zero responder/status, while a reply carries `READY`,
`NOT_READY` or `MISMATCH`. Strict lengths, canonical digest strings, source
identity and wire-6 admission are required. The complete payload is 158 bytes.

A query never initializes storage. INI-only nodes answer `NOT_READY`, and wire-5
sessions reject these types. Collection starts only through the internal
coordinator entry point after durable local initialization and exclusion of INI
reload; ordinary daemon startup does not trigger it yet. Retries share a fixed
ten-second deadline and cannot convert bootstrap status into readiness. Completed
collection now goes through the scheduler's barrier storage worker before local
configuration-vote eligibility is available. No application or
local CLI protocol version changes accompany this exchange.

Wire 6 also reserves `CONFIG_TX_CONTROL` (33), `CONFIG_TX_BEGIN` (34),
`CONFIG_TX_CHUNK` (35), and `CONFIG_TX_END` (36) for acceptor transactions.
Format-1 control frames are exactly 255 bytes and carry a kind/result, sender and
target daemon instances, request ID, revision, ballot, promised/accepted ballots,
and membership/parent/value digests. A PREPARE receives a PROMISE only after its
journal write completes. Successful ACCEPT and LEARN receive ACK and LEARNED,
respectively; non-success statuses distinguish rejection, stale history,
not-ready, busy and outcome-unknown. Outcome-unknown never implies abort.

Snapshot frames prefix the existing bounded transfer codec with an 18-byte
format/sender/target envelope. ACCEPT and LEARN are received; a PROMISE may send
its complete prior accepted snapshot. Source node identity must match the ballot
index. Streams reset on session teardown, and chunk/input/output limits and fixed
deadlines apply. No released HELLO or lease layouts change, and these frames are
never sent on wire 5. INI-only nodes refuse requests without creating history.

An internal network proposer can explicitly drive these existing frames through
prepare, accept and learning, correlating replies and complete PROMISE snapshots
with the request, ballot and admitted daemon instances. No additional frame type,
wire version or local CLI protocol change accompanies that coordinator. Unsolicited
replies do not count. Automatic activation and resource application remain unfinished.

Format-1 control kinds 5 (`SYNC_QUERY`) and 6 (`SYNC_STATUS`) add committed
checkpoint pulls using these same wire-6 frame IDs. Queries allow revision zero,
zero ballots, and bind the requester digest in `parent` and frozen baseline in
`value`. A successful STATUS carries the server's committed revision/ballot,
parent/value digests and zero promised/accepted fields; its bounded SYNC snapshot
carries the original majority mask and request ID. Revision-zero STATUS uses a
zero ballot and baseline digest as parent/value. Negative STATUS has a zero ballot
and empty value. Matching requests and peer instances are mandatory; unsolicited
snapshots cannot initialize state. Unlike a new PREPARE/ACCEPT/LEARN, a SYNC relay
may carry another member's original proposer ballot. The checkpoint action admits
validated revision gaps through the shared storage worker without applying resources.
All these additions remain within unreleased wire 6; the CLI protocol is unchanged.

Wire 6 also adds `RESOURCE_FRAME` (37) for dynamic resource-generation binding.
Its format-1 payload is a fixed 158-byte prefix (u16 format, u16 inner message
kind, u64 sender instance, u64 target instance, u64 applied revision, two canonical
65-byte membership/applied-value digests), followed by the unchanged inner body.
The outer sequence is preserved for request/reply matching. Inner kinds are
resource lease/state-sync/move/control messages, voting heartbeats and their
errors; HELLO, configuration messages and nested envelopes are forbidden. The
whole payload, including prefix, must fit the existing 64-KiB limit. Strict
instance/context checks run before dispatch and RPC completion. This remains
within unreleased wire 6 and does not change CLI protocol or released HELLO size.
Dynamic HELLO uses frozen baseline fingerprints/counts and cannot advertise voting
readiness; legacy INI HELLO fields keep their existing meaning. Dynamic authority
requires wire 6 even though legacy peering still supports wire 5.

Wire 6 also adds `CONFIG_OWNER_BARRIER` (38) for explicit old-owner stop proof.
The format-1 body is exactly 243 bytes: u16 format, u8 kind, u8 result,
u64 sender instance, target instance, round ID, old revision and target revision,
u32 dependency-drain mask, and three canonical 65-byte membership/old-value/
target-value digests. Kind 1 is QUERY with result zero; kind 2 is REPLY with
STOPPED (1), PENDING (2), or FAILED (3). Kind 3 is APPLIED_QUERY with result zero;
kind 4 is APPLIED_REPLY with APPLIED (1), PENDING (2), or FAILED (3). Stop replies
cannot satisfy application collection or vice versa. Both phases use distinct,
strictly increasing round IDs and fresh outer sequences.
Established full-member admission and negotiated wire 6 are mandatory. This raw
configuration channel remains available across resource-generation differences;
it is not an indexed resource envelope. INI-only nodes answer PENDING without
creating configuration history. Explicit collection still requires local cleanup
and every full member's stop proof; no query authorizes cleanup or activation.
Stop receipts retire only after every full member confirms the exact applied
scope. A retained completion scope answers slow consumers until every member
applies the next transition. Startup/catch-up recovery and automatic application
remain unfinished. These additions stay within unreleased wire 6 and do not
change released HELLO or the CLI protocol.

Wire 6 is temporarily labelled `unreleased` by `lcs_peer_protocol_release()`.
When releasing it for the first time, replace that label in `src/protocol.c` and
update the corresponding protocol-label assertions in
`tests/unit/protocol_negotiation.c` and `tests/integration/cases/json-output.sh`.
The existing wire-5 label remains `1.1.0`.

The additive local `config status` request/response use message types 14/15 and
their own versioned payload. Existing ordinary local status responses and their protocol
version are unchanged; old daemons reject the new request as unsupported.
Format 1 describes legacy INI preparation only and remains unchanged, including
peer bootstrap metadata. Activated local diagnostics use response format 2:
the same count/self/node records followed by a 479-byte local extension containing
u8 authority (1 = JSON), u8 admitted, u16 application phase, three u64 revisions
(committed/applied/pending), three canonical 65-byte digest strings and a 256-byte
zero-padded error string. Pending revision zero requires an empty pending digest.
Phases 1–6 are recovering, blocked, admitting, active, failed and stopping;
admitted is true only for active. Active/admitting require caught-up applied state
and no pending work. Old clients reject format 2 instead of reporting INI authority;
new clients decode both. These are local diagnostics, not peer readiness or
agreement evidence. The independent response format changes; peer wire version,
ordinary local status protocol version and release version remain unchanged.

## Local configuration export protocol

Local Unix-socket message types `CONFIG_EXPORT_REQ` (39) and `CONFIG_EXPORT_CHUNK`
(40) add a separately versioned read-only interface. They are not peer messages;
peer wire 5–6, ordinary local status protocol 1004 and the release version remain
unchanged. An older daemon returns an unsupported-request error.

A format-1 request is three bytes: u16 format and u8 selection (1 committed,
2 applied). Each response chunk has a 20-byte prefix: u16 format, u8 authority
(0 INI, 1 JSON), u8 selection, u64 revision, u32 total document bytes and u32 offset,
followed by nonempty document bytes. INI requires revision zero; JSON requires
a positive revision within the existing signed-64-bit resource revision limit.
The 512-KiB document and 64-KiB frame limits apply. Every chunk repeats the same
request sequence and pinned metadata, with contiguous offsets starting at zero.
The server closes after the complete document. Unknown formats/selections,
out-of-range totals/offsets and truncated/inconsistent streams fail. The client
buffers the bounded complete document before stdout, avoiding partial output on
transfer failure. This interface carries definitions, not commit/vote authority.

## Request-fact journal record format

The explicit `config_requests` core writes a private
`requests.json` format-1 record: `record_version`, local `member`, canonical
`membership`, a `requests` array and SHA-256 `checksum`. Entries contain
`request_id`, integer `base_revision`, canonical `base_digest`/`value_digest`
and boolean `committed`. IDs/base/value identities are immutable; worker imports
may only append identities or advance committed bits. The record is bounded to
128 identities and 128 KiB with no automatic eviction. Existing/corrupt history
is never replaced by initialization. Startup reads an existing record against the
frozen base and rejects orphaned, unsafe, corrupt, foreign or locally contradictory
history. Older catalogs with no record remain readable with history unavailable;
startup does not create empty history or infer missed outcomes. Reservation and
decision observation have explicit jobs on the shared storage worker; initialization
and transaction submission do not yet run automatically in the daemon. An explicit
open/recovery worker rechecks disk agreement before publishing history; missing
history can be created only with first-initialization authorization at pristine
revision zero without promises/acceptances/learning. Pristine state alone is not
authorization to recreate lost reservations. These are local reservation/decision
facts, not peer evidence or final client outcomes. There is
no wire, CLI, existing-record or release-version change. The transaction
coordinator has an internal request-storage phase and separate completion counter;
these are process-local bookkeeping and do not alter durable records or wire
messages. Coordinator OPEN only recovers existing history; it does not authorize
first initialization or enable public mutation commands.

## Offline document validation

Offline `lcs config validate` uses the existing schema-1/schema-2 parser and
selected INI context. It sends no messages and changes no wire, ordinary local
status, disk-record or release version. The CLI now links the already-required
Jansson runtime directly, without adding a new package dependency family.

## Application catalog record format

`application.json` now writes record format 2 with two complete predecessor
values in `history`, in addition to `applied` and `pending`. FINISH rotates these
values atomically. Recovery accepts record formats 1 and 2; format 1 has unknown
predecessor history and cannot be used to infer completed cleanup scopes. When
reading a nonzero format-1 applied value, export/BEGIN keep format 1 until a real
FINISH establishes a predecessor and writes format 2. No synthetic history is
created. Old binaries that cannot read format 2 must not replace it with legacy
INI state. This is a private disk/worker-handoff format; resource JSON schema,
peer wire versions, released HELLO and local CLI protocol do not change.
