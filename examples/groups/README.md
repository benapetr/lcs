This cluster consists of three nodes:

- `a` and `b` are full-members and can run VIPs.
- `c` is quorum-only and votes, but never runs VIPs.

The example defines two VIPs in a `keep-together` / `best-effort` group. LCS prefers both VIPs on the same full-member. If they ever become split across full-members, automatic rebalance moves the lower-priority VIP to the owner of the highest-priority VIP using the normal safe move / lease handoff path.

To try anti-affinity instead, change the `group = vips` lines under both VIPs to `group = anti`. With `anti-affinity` / `strict`, LCS keeps the VIPs on different full-members and may leave the lower-priority VIP stopped if there are not enough online full-members.

Copy the matching node config as `/etc/lcs/lcs.conf` on each node.
