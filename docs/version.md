# Version update locations

Update these files when bumping the LCS release version:

- `src/common.h`
- `packaging/lcs.8`
- `packaging/lcsd.8`

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

Version 5 establishes this negotiation contract. It intentionally does not
decode the earlier version-3 HELLO because no release was made between those
formats.
