# Resource JSON format and bootstrap implementation

This is the first implementation stage of
[dynamic distributed configuration](dynamic-config-plan.md). Resource JSON is
currently a prepared local snapshot. **INI remains authoritative, wire protocol
remains version 5, and runtime definition commands are not enabled yet.**
Distributed agreement, durable activation, peer synchronization, and runtime
mutations must be implemented before JSON can become authoritative.

## Storage and upgrades

Starting `lcsd` automatically prepares `/etc/lcs/resources.json` from the effective
groups and resources in `/etc/lcs/lcs.conf`. It leaves the INI file untouched.
It also creates `/var/lib/lcs/bootstrap.state`, recording initialization and
cluster identity, and holds an exclusive `/var/lib/lcs/lock` while running.
Packages create `/var/lib/lcs/` with mode `0700` before service restart; generated
files use mode `0600`. Jansson (MIT license) is the JSON parser; OpenSSL libcrypto
provides SHA-256 for agreement identities and journal checksums. Development
builds require `libjansson-dev` and `libssl-dev` on Debian/Ubuntu, or
`jansson-devel` and `openssl-devel` on RPM systems, plus `pkg-config`.

A custom configuration `/path/node.conf` uses `/path/node.conf.resources.json`
and `/path/node.conf.state/`. Paths are derived from the resolved INI filename,
so relative aliases use the same lock. Multiple configurations in one directory
have independent state. The daemon user must own both directories, which must
not be writable by group or others. Direct execution creates the state directory
without requiring systemd; its parent must already exist.

Successful coordinated INI reloads schedule a background snapshot refresh.
Writes and filesystem syncs run outside the lease-processing event loop.
Concurrent refresh requests are serialized, with the latest applied INI
configuration prepared after the current worker completes. Refresh failure is
logged and does not undo an already committed legacy reload.

An interrupted first initialization can resume automatically. Once initialization
is recorded as prepared, missing or invalid JSON causes startup to fail with a
diagnostic instead of silently reimporting INI. An unknown initialization record,
cluster mismatch, or nonzero JSON revision is also rejected by this bootstrap-only
implementation. These conditions require investigation/recovery, not deletion of
state files to force conversion. Packages do not overwrite generated snapshots
or remove populated state directories during normal upgrades/removal.

The running daemon currently writes bootstrap metadata only. A durable agreement
journal and worker are implemented separately but are not connected to daemon
activation yet; see [durable agreement](dynamic-config-agreement.md). Local atomic
replacement is not evidence of cluster agreement. Files are synced before
replacement, followed by a directory sync; a failed sync after replacement is
reported as an uncertain outcome.

## Schema version 1

```json
{
  "schema_version": 1,
  "cluster": "production",
  "revision": 0,
  "groups": {
    "web": {
      "type": "keep-together",
      "mode": "best-effort"
    }
  },
  "resources": {
    "public-vip": {
      "type": "vip",
      "address": "192.168.10.100/24",
      "interface": "eth0",
      "group": "web",
      "priority": 2,
      "home_node": "node1",
      "depends_on": []
    },
    "nginx": {
      "type": "service",
      "systemd_unit": "nginx.service",
      "group": "web",
      "priority": 1,
      "depends_on": ["public-vip"]
    }
  }
}
```

All five top-level fields are required. `cluster` must match the local static
configuration. `revision` is an integer from zero through `9223372036854775807`;
zero is reserved for bootstrap. Unknown schema versions and unknown fields are
rejected. Empty `groups` and `resources` objects are valid.

Group names and resource names are object keys and follow the existing INI name
and length restrictions. Groups require `type` (`keep-together` or
`anti-affinity`) and `mode` (`strict` or `best-effort`). Resources require `type`
(`vip` or `service`) and the existing backend-specific mandatory fields.

Supported resource properties are `address`, `interface`, `systemd_unit`, `group`,
`home_node`, `priority`, `depends_on`, `pre_start`, `post_start`, `pre_stop`, and
`post_stop`. VIPs cannot specify `systemd_unit`; services cannot specify `address`
or `interface`. Hooks must be empty or absolute paths. `priority` is a positive
32-bit integer; omission retains the existing sorted-order default.
`depends_on` is an array of unique resource names; omission means no dependencies.
The common configuration validator resolves groups, home nodes, dependencies,
unique priorities, and dependency cycles before accepting a candidate.

Documents are limited to 512 KiB and existing compiled group/resource/dependency
limits. Duplicate object keys, embedded null characters, invalid UTF-8, trailing
data, and incorrect value types are rejected. Decoding constructs a candidate
without changing the running configuration or importing daemon settings.

Encoding sorts object keys and dependencies and writes indented JSON. Derived
runtime indexes are not serialized. Files do not contain node credentials,
leases, ownership, epochs, or temporary administrative stop flags. The canonical
snapshot preserves resource interfaces and hook paths even though the legacy
peering fingerprint intentionally omits those node-local fields; future
activation must explicitly resolve any such differences between nodes.

## Candidate editing layer

The internal candidate builder now accepts the following argument forms. These
are tested editing operations, **not yet commands exposed by the live `lcs` CLI**.
They construct a replacement candidate, validate it as a whole, and leave their
inputs and output unchanged on failure. Applying a candidate still requires the
future distributed agreement and resource-transition implementation.

```text
resource create NAME vip address=CIDR interface=INTERFACE [key=value ...]
resource create NAME service systemd_unit=UNIT [key=value ...]
resource update NAME key=value ...
resource delete NAME
resource remove NAME
resource group add GROUP RESOURCE ...
resource group remove GROUP [RESOURCE ...]
resource group delete GROUP [RESOURCE ...]
resource group create GROUP [type=TYPE] [mode=MODE]
resource group update GROUP key=value ...
```

Resource creation/update/deletion and group membership forms follow the structure
of the [upstream pcs command definitions](https://github.com/ClusterLabs/pcs/blob/main/pcs/usage.py).
Empty update values remove an option; `depends_on` uses comma-separated names.
Repeated/unknown options, stale expected revisions, missing required backend
fields, and invalid resulting dependencies are rejected. Resource type cannot
be changed through update. Deleting a resource does not implicitly delete its
dependents. No `--no-stop` or forced unsafe deletion is supported.

Group `add` creates the group when absent, using `keep-together` and `best-effort`
defaults, and assigns existing resources to it. Group `remove`/`delete` with no
resource arguments ungroups its members and removes the group without deleting
resources. With resource arguments it removes only those memberships, deleting
the group if it becomes empty. Group `create` and `update` are LCS extensions for
explicit group definitions and policies, including `anti-affinity` and `strict`.
LCS group placement does not imply Pacemaker's ordered-group semantics or
arbitrary Pacemaker constraints/resource agents. Group placement priority remains
the resource's `priority` field; it is not inferred from CLI argument order.
