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

This is an acceptor/learner endpoint. An automatic network proposer, correlation
of incoming PROMISE snapshots into its quorum, multi-revision synchronization and
resource application remain unfinished. Unsolicited control replies are not counted
as votes, and incoming PROMISE/SYNC transfers are not accepted by this endpoint.

Wire 6 is temporarily labelled `unreleased` by `lcs_peer_protocol_release()`.
When releasing it for the first time, replace that label in `src/protocol.c` and
update the corresponding protocol-label assertions in
`tests/unit/protocol_negotiation.c` and `tests/integration/cases/json-output.sh`.
The existing wire-5 label remains `1.1.0`.

The additive local `config status` request/response use message types 14/15 and
their own format-1 payload. Existing local status responses and their protocol
version are unchanged; old daemons reject the new request as unsupported.
Format 1 describes legacy INI preparation only. Future active-authority status
must explicitly encode that authority in a new format, rather than reusing this
bootstrap-only response and letting an older client report INI authority.
