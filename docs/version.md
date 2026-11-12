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

A readiness query never initializes storage. INI-only nodes answer `NOT_READY`, and wire-5
sessions reject these types. Collection starts only through the internal
coordinator entry point after durable local initialization and exclusion of INI
reload; ordinary daemon startup does not trigger it yet. Retries share a fixed
ten-second deadline and cannot convert bootstrap status into readiness. Completed
collection now goes through the scheduler's barrier storage worker before local
configuration-vote eligibility is available. No application or
local CLI protocol version changes accompany this exchange.

`CONFIG_FREEZE_QUERY` (47) and `CONFIG_FREEZE_REPLY` (48) add the separate
pre-initialization exchange. Their format-1 payload is also exactly 158 bytes:
u16 format, u8 kind, u64 requester instance, u64 increasing attempt, u64 responder
instance, u8 status, and two canonical 65-byte membership/baseline digests.
Unlike readiness queries, freeze queries explicitly name a nonzero destination
instance and have status zero. Replies retain that tuple with `FROZEN`, `BUSY`
or `MISMATCH`. The type and kind must agree; malformed, foreign-instance and
wire-5 requests are rejected before storage. Full and quorum-only members may
collect confirmations for their local initialization. Both must participate;
quorum-only nodes still cannot initiate resource definition mutations.

An INI responder starts the shared freeze writer only after every configured
member has an established upgrade-capable session and local reload/refresh work
is excluded. FROZEN requires successful durable worker publication, not file
visibility or version advertisements. Bounded waiters correlate attempts and
instances, do not extend deadlines on exact retries, and drop obsolete replies.
Writer work survives requester loss; deadline/cancellation never thaws storage.
The collection-only entry requires its own durable local freeze and never
initializes or switches authority by itself. A separate daemon workflow now
resumes existing durable freezes through fresh all-member collection and local
initialization. Ordinary INI startup does not create the first freeze from version
advertisements; automatic first-freeze/JSON activation remains unfinished.
Fenced frozen-startup resumption uses only wire-6 coordination. HELLO layouts,
released wire-5 messages, local protocol and release version
are unchanged; these types belong to unreleased wire 6.

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
new clients decode both. Format 3 extends format 2 by one required u8
`dynamic_commands` equal to 1. It is emitted only for admitted full members with
activated JSON authority and healthy request history. Other views retain formats
1/2. Readers reject format 3 with a zero/invalid flag or an inactive view; older
clients reject it instead of reporting a false capability. The flag is local
admission capability, not current quorum or an idle-writer guarantee. These are local diagnostics, not peer readiness or
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
`requests.json` with record formats 1/2/3: `record_version`, local `member`, canonical
`membership`, a `requests` array and SHA-256 `checksum`. Entries contain
`request_id`, integer `base_revision`, canonical `base_digest`/`value_digest`
and boolean `committed`. Format 2 adds required boolean `applied`, which can only
be true when committed is true. Format 3 adds required `client_digest`: either an
empty string (client descriptor unknown) or a lowercase SHA-256 fingerprint.
Readers accept formats 1/2 without inventing client evidence, and format 1 with
admission unknown (false), without rewriting on read. Subsequent legitimate writes
use format 3. A descriptor, including its absence, is immutable once recorded.
IDs/base/value identities are immutable; worker imports may only append identities
or advance committed/applied bits. The record is bounded to
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
and admission facts, not peer ownership evidence or cluster-wide client outcomes.
Record format 3 and applied-response format 2 are deliberate additions; ordinary
local status, peer wire versions and release version stay unchanged. The transaction
coordinator has an internal request-storage phase and separate completion counter;
these are process-local bookkeeping and do not alter durable records or wire
messages. Coordinator OPEN only recovers existing history; it does not authorize
first initialization or enable public mutation commands. The internal proposer
now requires healthy existing history and persists original request intent before
PREPARE; its private reservation phase uses request completions separately from
votes. Known request IDs cannot be submitted again by changing the transport ID.
ACCEPT now persists/checks the selected identity before persisting acceptance;
its private coordinator phase does not report a vote or send ACK until the latter
write completes. Exact protocol retries remain idempotent; conflicts and a full
history reject new identities. Successful LEARN/checkpoint persistence schedules
observation of the exact current decision when history exists, using a private
coordinator phase. Consensus completion remains known across observation failure;
configuration progress is fenced for recovery. Missing history and skipped
checkpoint outcomes remain unknown. This adds no wire fields or disk-format
changes. Public mutation payloads and their origin client outcomes are described below.

## Local request-fact query

`lcs config request-status ID` uses additive Unix-socket message types 41/42 with
payload format 1. The request is u16 format plus a canonical zero-padded 64-byte
request ID (66 bytes). The response is u16 format, u8 history, u8 request state,
64-byte ID, u64 expected base revision and two 65-byte canonical digest strings
(206 bytes). History values 0/1/2 mean unavailable/available/recovery-required;
request states 0/1/2 mean unknown/reserved/committed. Response format 2 adds state
3 (applied) with the same 206-byte layout. The server retains response format 1
for the older states, while an applied receipt uses format 2 so old clients reject
it rather than misreport success. New clients read both formats; state 3 is
invalid in format 1. The request remains format 1. Known states require available
history, a base revision below the resource maximum and lowercase SHA-256 digests.
Unknown states require zero revision and empty canonical digest fields. Truncation,
trailing bytes, invalid states/IDs/digests and nonzero padding are rejected.

The client also checks response sequence and request-ID correlation before output.
JSON uses null revision/digest fields for unknown records and always marks the
scope local. Exit 0 means a valid query response, including unknown; it does not
mean a client mutation succeeded. Older daemons report unsupported requests.
Ordinary local status protocol 1004, peer wire versions, resource JSON schema
and release version are unchanged. Request record formats 1/2/3 are described above. There is no peer request-status endpoint.

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


## Local definition mutation payloads

Unix-socket-only types 43/44 are configuration mutation request/response. Payload
format 1 is independent of peer wire 5/6 and ordinary status protocol 1004.
A request contains u16 format, zero-padded request ID (64 bytes), u8 optional-base
flags (bit 0 revision, bit 1 digest), u64 expected revision, zero-padded expected
digest (65 bytes), u16 argc, then argc arguments, each u16 byte length followed by
exact non-NUL bytes. Missing revision/digest fields must be all zero. Explicit
revision must be below INT64_MAX; explicit digest is lowercase SHA-256. Requests
are bounded to one 64-KiB frame, 3–64 arguments and `LCS_PATH_MAX + 32` bytes per
argument. Unknown flags/formats, truncation, noncanonical padding, embedded NUL
and trailing bytes fail. Argument strings are bounded before candidate validation.

A response is exactly 465 bytes: u16 format, u8 outcome (0 applied/1 rejected/
2 pending/3 unknown), a 206-byte versioned local request-fact view, and a required
nonempty zero-padded 256-byte reason. Applied requires an applied fact; pending
requires committed; rejected has no known fact (it does not report the fate of a
conflicting older request). The CLI validates sequence, response type, format,
canonical facts and exact ID before producing a result. Invalid/unsupported or
uncorrelated responses retain the original ID with unknown outcome/exit 3.
There is no peer mutation endpoint or protocol-version bump. CLI flags, message
types and local config-status format 3 are additive unreleased features; release
version and resource JSON schema are unchanged.


## Local atomic import payloads

Unix-socket-only types 45/46 are import BEGIN/CHUNK with payload format 1. BEGIN
is at most 256 bytes: u16 format, u32 total document bytes (1–524288), followed by
an ordinary format-1 mutation descriptor with exactly the arguments `config`,
`import`, `json`. This placeholder is validated and never becomes the origin
fingerprint. The server hashes exact completed document bytes for that descriptor.

Each CHUNK contains u16 format, u32 offset and nonempty data, fitting one 64-KiB
frame. All chunks share BEGIN's sequence and must be contiguous from zero without
duplicates, gaps, interleaved commands or overflow of the announced total. The
exact final byte completes the upload; there is no separate END or reservation
before complete validation. Upload has a fixed ten-second deadline. A partial
connection cannot leave durable import staging. The final response uses the
existing mutation type 44 and result format 1, correlated to BEGIN's ID/sequence.
These are additive local formats, with no peer wire, journal record, resource
schema, ordinary status or release-version change.


## Mutation diagnostic text

Mutation result format 1 retains its existing outcome/fact fields and bounded
256-byte `reason`. Blocked-attempt cleanup/projection details and fatal
configuration-storage recovery instructions refine that text; no result code,
wire/local format, disk schema or release version changes. Reasons are diagnostic
text, not a machine-stable error-code enumeration. Exact origin retries and
waiters keep the same pending/unknown semantics, and historical applied facts are
never downgraded because a later operation fails.


## Initialization intent record format 2

`activation.json` format 2 retains the exact format-1 keys and checksum algorithm:
`record_version`, `phase`, `member`, `membership`, `baseline_digest`, `baseline`
and `checksum`. The version itself requires request history. Its durable order is
INITIALIZING intent, agreement, application, empty `requests.json`, then PREPARED
intent, with file and directory sync for each replacement (ten sync boundaries).
A required history file may be created on resume only while INITIALIZING, with
pristine baseline agreement/application and no barrier or request reservations.
PREPARED requires an existing valid request journal, even at revision zero.

The shared initialization worker creates format 2 and verifies empty history
before publishing its preallocated handles through the same-host acknowledgement.
It refuses legacy format-1 intents, missing/corrupt PREPARED history and nonempty
reservations rather than synthesizing an empty journal. Normal runtime recovery
still accepts format 1 without read-time rewrite or invented history; its legacy
low-level creation primitive remains available for compatibility fixtures. Format
2 recovery includes newly completed INITIALIZING history in its returned runtime,
not just files which existed before recovery began. Older readers reject format 2.
This is a private intent-record change, not a peer/CLI wire, resource-schema or
release-version change. Automatic all-member activation remains unfinished.

## Reload freeze intent record format 3

`activation.json` format 3 uses the same exact keys and checksum as formats 1/2,
but permits only phase `frozen`. It records local durable reload exclusion and
the complete legacy baseline before all-member initialization authorization.
Agreement, application, requests and barrier files must all be absent. It does
not authorize creating any of them. Exact freeze retries and startup recovery
read without rewriting; mismatched baselines or contradictory history fail.

The shared freeze worker syncs the file and directory before parent publication
and acknowledgement (two sync boundaries). After explicit coordinated
authorization, promotion syncs a format-2 INITIALIZING intent and follows the
existing initialization order (ten further boundaries). A FROZEN restart never
promotes from disk state alone; its resumed coordinator must obtain fresh
all-member confirmation. Older readers reject format 3 instead of importing INI.
Healthy frozen startup can resume coordination with resource voting/placement
fenced, without rewriting the record or creating journals; contradictory history
still fails before listeners. This private storage format does not change the
resource schema or release version. Automatic activation remains unfinished.

## Frozen configuration diagnostic format 4

Local `config status` types 14/15 add diagnostic format 4 for a durable FROZEN
runtime or pristine mandatory-history revision-zero PREPARED state. It retains
the member-view prefix and format-2 extension layout, but
requires INI authority, phase `frozen` (7), admitted zero, zero revisions and empty
committed/applied/pending digests. Dynamic command capability is absent/false;
the local shadow-prepared flag must be false. Error text explains the pending
coordination. It is not an activation or lease-voting proof.

Legacy INI remains format 1 and activated JSON remains format 2/3. Older clients
reject format 4 rather than reporting a frozen node as ordinary active INI.
The CLI renders `application_state: frozen` and `admitted: false`; read-only
export remains schema-1/revision-zero baseline data. No peer message layout or
ordinary local protocol number changes. Cold frozen startup permits only wire-6
HELLO admission and repeats unchanged advisory status for liveness while resource
frames remain fenced.

Existing durable freezes now resume per-member initialization through the same
wire-6 freeze query/reply format. A healthy pristine prepared member may reaffirm
FROZEN while lacking any promise, accepted successor, barrier, reservation or
chosen decision; more advanced state returns BUSY rather than being reinitialized.
The coordinator uses a separate shared-writer INITIALIZING phase and process-local
completion counter, returning to IDLE without configuration voting authority.
Cold prepared restarts preserve journals and restore resource fences. These
workflow changes add no wire/storage formats or release-version bump. First-freeze
creation from INI and JSON activation remain unfinished.

Automatic readiness now follows pristine mandatory-history preparation after an
existing durable freeze, using existing wire-6 types 31/32 and barrier format 1.
Failed/expired unsealed in-memory collection may retry with a new attempt; no
retry resets an active writer, poisoned storage or uncertain persistence.
Pristine revision-zero preparation with a valid barrier resumes the coordinator
in READY without disk rewrites, under unchanged resource voting/placement fences.
Diagnostic format 4 still reports frozen INI authority, no admission/revisions or
dynamic commands, with readiness-persisted text while awaiting activation.
These workflow changes require no wire/storage format or release-version bump.

Undecided activation recovery also retains revision-one promises, request
reservations and a validated accepted schema-2 activation value over the frozen
revision-zero baseline. Mandatory healthy history and the existing all-member
barrier are required; an accepted value must have its exact request reservation.
Startup restores configuration coordination without rewriting records, clearing
promises, inferring a decision or admitting resources. Diagnostic format 4 and
wire-6-only frozen peering remain unchanged. Missing barrier/history fails closed;
learned activation and initial application remain unfinished. No protocol,
storage-format or release-version change is required.

## Initial activation diagnostic format 5

Local config-status types 14/15 add format 5 with the existing format-2 extension
layout for a learned revision-one activation while revision zero remains applied.
It requires JSON authority, committed revision 1, applied revision 0, valid learned
and baseline digests, no admission or dynamic command capability, and recovering,
blocked, failed or stopping phase. Pending revision is absent or exactly 1 with
the committed digest. Active/admitting baseline state is invalid. Formats 2/3
continue requiring nonzero applied revisions; older clients reject format 5.
After FINISH, ordinary format 2/3 diagnostics resume. Frozen undecided state stays
format 4. Peer wire 6, peer message layouts, disk formats, the ordinary local
protocol number and release version remain unchanged.

Chosen activation recovery uses the existing serialized application, cleanup,
old-owner, projection and admission sequence. It preserves the majority decision,
records baseline predecessor history at FINISH, and does not fabricate a dynamic
revision-zero generation. Crash/restart after BEGIN or failed cleanup retains
all fences until safe application and fresh sync/quorum admission. Automatic
first-freeze/proposal creation and live activation handoff remain unfinished.

Live frozen nodes also hand a learned revision-one activation into the existing
recovery/application controller without restarting. The adapter retains daemon
sessions, coordinator/transaction state, epochs, private promises and table
indices; it waits for the serialized writer and ordinary workers/ownership to
drain before acquiring the application hold. Exact frozen definitions and global
resource fences are mandatory. Admission still requires cleanup, old-owner proof,
FINISH and fresh generation sync/quorum. This adds no wire, local diagnostic,
storage format or release-version change. First-freeze and activation-proposal
creation from ordinary INI remain unfinished.

Automatic proposal creation now follows a durable readiness barrier on existing
frozen nodes. The preferred connected full member submits the unchanged baseline
as a schema-2 revision-one successor through the existing transaction protocol.
Fresh instance/attempt request IDs preserve earlier unknown reservations; a new
higher-ballot phase one can recover earlier accepted values. Valid correlated
rejection hints raise only the next ballot; they never count as votes or rewrite
an active proposal. Full/poisoned history blocks submission without eviction.
This uses existing wire 6, local diagnostic formats, request journal and storage
formats and does not bump the release version. Ordinary INI first-freeze creation
and live-owner draining remain unfinished.

A durable freeze on a running INI node now triggers a fenced local resource drain
before FROZEN acknowledgement or local initialization. Fenced resource RPCs are
rejected before registration, preserving the separate configuration connection
when backend completion attempts a legacy release. Existing wire-6 freeze
query/reply fields and status codes are unchanged: BUSY remains the response until
old backend/hook workers, grants and ownership drain; failed cleanup cannot return
FROZEN. Local format-4 diagnostics can report the exact cleanup failure. The drain
preserves indices, epochs, private promises and administrative state, and does
not fabricate distributed owner proof or initialize history. Quorum-only nodes
perform no backend work. This adds no wire/storage format, ordinary local protocol
or release-version change. Automatic first-freeze initiation remains unfinished.

The unreleased wire-6 freeze reply now adds LOCKED (status 4) to its existing
format-1, 158-byte body (types 47/48). This is a semantic extension of unreleased
wire 6, with no new layout, storage record, local protocol or release-version
change. LOCKED proves only the durable reload/acquisition lock. A separate fresh
all-member round, including quorum-only members, must confirm every lock before
local owners drain. Exact unexpired old leases may renew during collection; new
acquisitions, replacement identities and expired renewals are refused. FROZEN
still requires drained owners, and only FROZEN can authorize initialization.
Wire 5 is refused immediately after durable locking, even before backend drain.
Automatic first-freeze initiation from ordinary INI startup remains unfinished.
