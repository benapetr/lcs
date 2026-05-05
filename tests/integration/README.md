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
