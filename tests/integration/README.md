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
state. The test covers legacy-mode upgrade and JSON preparation, not the complete
distributed migration/activation path. It starts a new
full member with old full/quorum-only peers, upgrades the peers one at a time,
checks wire-5/6 negotiation and unknown old metadata, and verifies that ownership
and resource epoch are preserved. Dynamic commands remain disabled throughout.
