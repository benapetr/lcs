This example runs a complete 3-node LCS cluster on one machine.

It uses:

- `node1` full-member on `127.0.0.1:3331`
- `node2` full-member on `127.0.0.1:3332`
- `node3` quorum-only on `127.0.0.1:3333`
- one loopback VIP: `127.0.0.200/32` on `lo`
- separate local CLI sockets:
  - `/tmp/lcs-node1.sock`
  - `/tmp/lcs-node2.sock`
  - `/tmp/lcs-node3.sock`
- separate Prometheus ports:
  - `127.0.0.1:9131`
  - `127.0.0.1:9132`
  - `127.0.0.1:9133`

Example run commands from the repository root:

```sh
sudo ./lcsd --no-syslog --config examples/localhost_cluster/node1.conf
sudo ./lcsd --no-syslog --config examples/localhost_cluster/node2.conf
sudo ./lcsd --no-syslog --config examples/localhost_cluster/node3.conf
```

Check status through any node socket:

```sh
./lcs status -s /tmp/lcs-node1.sock
./lcs status -s /tmp/lcs-node2.sock
./lcs status -s /tmp/lcs-node3.sock
```

Move the VIP between full members:

```sh
./lcs resource move vip1 node2 -s /tmp/lcs-node1.sock
./lcs resource move vip1 node1 -s /tmp/lcs-node2.sock
```

The VIP uses `lo`, so ARP/ND conflict checks and announcements are skipped.
