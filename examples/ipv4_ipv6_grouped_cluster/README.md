This example is a 3-node cluster with two full members (`a`, `b`) and one
quorum-only node (`c`). It manages one IPv4 VIP and one IPv6 VIP in a
best-effort keep-together group, so both VIPs normally run on the same full
member and move together.

The addresses use documentation-only prefixes:

- peer transport: `192.0.2.0/24`
- IPv4 VIP: `198.51.100.50/24`
- IPv4 gateway used by the hook example: `198.51.100.1`
- IPv6 VIP: `2001:db8:100::50/64`

Copy the matching `node_*.conf` to `/etc/lcs/lcs.conf` on each node and adjust
the peer addresses, VIP addresses, interface, gateway, and hook paths.

The sample hooks in `hooks/` are referenced as:

- `/usr/local/sbin/vip-public-up.sh`
- `/usr/local/sbin/vip-public-down.sh`

Install them at those paths or update the `post_start` and `post_stop` keys in
the VIP config.
