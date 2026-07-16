This example runs a 3-node cluster with one systemd service resource and
one VIP resource grouped together.

It uses:

- `a` full-member on `192.168.0.1`
- `b` full-member on `192.168.0.2`
- `c` quorum-only voter on `192.168.0.3`
- one service resource: `web` managing `nginx.service`
- one VIP resource: `web-vip` using `192.168.0.100/24` on `eth0`
- `a` as the home node for both resources
- a `keep-together` group so the VIP and service run on the same full-member
- `depends_on = web-vip` so LCS starts the VIP before `nginx.service` and stops
  `nginx.service` before removing the VIP

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

Adjust the `web-vip` address and interface for your network before using the
example. Then start `lcsd` on all nodes. The service and VIP normally start on
node `a`. If `a` fails, the leases expire and another full-member can start the
unit and bring up the VIP. When `a` returns, LCS moves both resources back home
unless a manual move away from home has blocked automatic return.

Useful commands:

```sh
lcs resource list
lcs resource move web b
lcs resource stop web
lcs resource start web
```
