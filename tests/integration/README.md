# LCS Integration Tests

These are opt-in localhost regression tests for cluster behavior that is hard to
cover with unit tests.

They:

- build `lcsd` and `lcs` if needed;
- generate temporary configs and Unix sockets;
- use random localhost TCP ports;
- run daemons with `LCS_VIP_DRY_RUN=1`, so they do not require root and do not
  modify real interface addresses;
- store daemon logs in a temporary directory and print them on failure.

Run all tests:

```sh
tests/integration/run.sh
```

Run selected tests:

```sh
tests/integration/run.sh remote-move failover
```

These tests are intentionally not part of `make` because they start multiple
daemons and depend on localhost networking behavior.

Deterministic lease, grant, recovery-timer, and state-synchronization tests are
kept in `tests/unit` and can be run with:

```sh
tests/unit/run.sh
```

## Rolling wire-5 compatibility

The normal suite includes prepared-snapshot metadata, local configuration status,
reload refresh, and metadata invalidation after disconnect. To test unchanged
HELLO and lease behavior against an actual pre-wire-6 executable, build a separate
checkout (for example commit `dd78016`) and run:

```sh
LCS_WIRE5_DAEMON=/path/to/old/lcsd tests/integration/compatibility/wire5-upgrade.sh
```

For released v1.1.1, which predates source filtering, use:

```sh
LCS_WIRE5_DAEMON=/path/to/v1.1.1/lcsd LCS_WIRE5_HAS_ALLOW_FROM=0 \
  tests/integration/compatibility/wire5-upgrade.sh
```

That mode omits `allow_from` on the old nodes and adds it before starting their
upgraded daemons. It also checks that the released nodes begin without dynamic
state. The test preserves legacy ownership and epoch while an old member remains,
then checks automatic conversion and JSON admission after the final upgrade.
Quorum-only members participate in migration without hosting resources or enabling
local definition commands.

Cases specifically testing pre-upgrade INI reloads use a cached current-source
wire-5 fixture. Only its advertised maximum peer version changes; normal cases
use the production wire-6 daemon, with no production migration bypass.
