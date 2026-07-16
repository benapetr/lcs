This example runs a 3-node cluster with one systemd service resource.

It uses:

- `a` full-member on `192.168.0.1`
- `b` full-member on `192.168.0.2`
- `c` quorum-only voter on `192.168.0.3`
- one service resource: `web` managing `nginx.service`
- `a` as the service home node

Build LCS with systemd D-Bus support:

```sh
make WITH_SYSTEMD=1
```

Install the same config on each node, changing only the `[cluster] node`
value as shown in the three example files.

The managed unit should not be enabled to start by itself on all nodes. Let
LCS own start and stop decisions:

```sh
systemctl disable --now nginx.service
```

Then start `lcsd` on all nodes. The service normally starts on node `a`.
If `a` fails, the lease expires and another full-member can start the unit.
When `a` returns, LCS moves the service back home unless a manual move away
from home has blocked automatic return.

Useful commands:

```sh
lcs resource list
lcs resource move web b
lcs resource stop web
lcs resource start web
```
