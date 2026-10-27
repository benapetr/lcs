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
still requires the future explicit durable barrier from every configured member.

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
