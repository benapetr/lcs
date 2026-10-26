# Configuration application and recovery contract

Status: design contract for the remaining implementation. The running daemon
still uses INI authority and wire 5. This document does not enable dynamic edits.
See [the agreement core](dynamic-config-agreement.md) and
[implementation checklist](dynamic-config-plan.md).

## Resource identity and message admission

A resource name is its administrative identity. Its backend incarnation is the
pair `(name, birth_revision)` within the activated configuration history.
Bootstrap resources have birth revision zero. Creation and replacement of a
backend allocate the revision in which that incarnation first appears.
Deleting and recreating a name must allocate a new birth revision even when its
backend properties are identical. Changing a VIP address/interface, service
unit, or resource type is a backend replacement. Policy, priority, dependency,
group and hook edits preserve incarnation, subject to safe application below.

The committed dynamic snapshot must carry these identities, including during
full-snapshot catch-up. Inferring identity from just the old and new endpoint
definitions is insufficient: a node can miss deletion and recreation between
two identical definitions. The existing schema-1 shadow JSON has no incarnation
metadata. The implemented schema-2 library carries and validates these identities;
its daemon activation and resource-message gates remain to be implemented.
The coordinator allocates identities; acceptors check them against the immediate
predecessor. Clients cannot supply or reuse birth revisions.

Resource array indices are local sorted-table offsets, not durable identities.
For active dynamic sessions, every resource message must be bound to the applied
configuration digest and incarnation, either in a new envelope or an explicitly
negotiated immutable session context. This includes lease requests/replies,
renewals, release, state sync, administrative controls and delayed worker results.
An index without a matching context cannot address a resource. Closing sessions,
cancelling indexed operations and discarding queued frames precede remapping.
Wire-5 sessions may be used during legacy upgrade, but cannot vote or host under
dynamic authority. A reconnect must not downgrade the authority or feature gate.

Preserve runtime ownership/grants only for a matching incarnation and backend.
Configuration revision changes alone do not increment resource epochs. New
incarnations start without ownership or inherited grants, and old messages cannot
populate them. Dependencies are resolved again against the new sorted table.

## Decision, drain and application ordering

1. Validate the complete candidate and expected predecessor, including identities,
   dependencies and static membership. Serialize activation, INI reload and dynamic
   edits; only one may transition local resource definitions at a time.
2. Run durable prepare/accept agreement. Persist promises and accepted values
   before their replies. No backend changes occur merely because a proposal was
   received or an acceptor voted. Once a value is chosen it cannot be rolled back
   because application failed; finish learning and repair application instead.
3. Persist the learned decision before reporting it committed. Publish
   `resources.json` as a projection of that decision. Do not start new backends
   yet. A projection write failure leaves a committed but pending operation.
4. Before any local backend operation, persist the last-applied definitions,
   incarnations and the complete cleanup obligations for the transition. This
   catalog must include the old backend details and stop hooks, rather than only
   resource names. Retain it until cleanup and an applied marker are durable.
5. Suspend grants/acquisition for affected resources and their transitive
   dependents. Stop dependents before dependencies, using the old definitions
   and stop hooks. Backend replacement and deletion require cleanup. Dependency
   edits require dependency readiness to be reevaluated before dependent restart.
   Policy/group changes may require placement reconciliation. Stop completion,
   outstanding hooks, probes, handoffs and backend workers are part of the barrier.
6. Establish that every old owner affected by the transition has stopped, or
   that its old lease has expired under the existing lease timing/recovery rules.
   A disconnected owner does not count as stopped. A known or suspected failed
   backend stop requires confirmed cleanup or external fencing; waiting alone
   cannot prove removal of an address or termination of a service. Keep affected
   activation blocked on `stop_failed`; do not override it through a majority.
7. Remap runtime state only after the local drain and old-generation ownership
   barrier. Preserve unaffected incarnations and rebuild all index references.
   Persist the applied snapshot/marker before admitting normal votes under the
   new generation. Start eligible resources in dependency order under new leases.
8. Mark the request applied once the coordinator's application barrier has
   succeeded: every participating online member has applied, and excluded owners
   have satisfied step 6. Applied means definitions installed and cleanup complete,
   not that every service health check succeeded or every resource is running.

The existing INI reload retains its existing all-participant drain-before-apply
behavior during rolling upgrade. It does not become a durable dynamic transaction
by reusing its in-memory fingerprints. Shared validation and transition planning
must not silently weaken its current barriers.

## Restarts, catch-up and missing files

| Durable state at interruption | Required recovery |
| --- | --- |
| Promise or accepted value, no known decision | Recover agreement; a replacement proposer must carry forward any chosen value. Do not assume abort. |
| Decision committed, projection stale/missing | Verify the local journal/history and rewrite the projection; never reload resource authority from INI. |
| Cleanup catalog persisted, backend action interrupted | Retry/probe cleanup conservatively using recorded old definitions. Do not vote or host affected resources until resolved. |
| Cleanup finished, applied marker not persisted | Repeat idempotent reconciliation; do not infer application from a missing backend alone. |
| Applied marker persisted | Recover the matching incarnation catalog, perform ordinary lease restart recovery, then rejoin resource voting. |
| Known active journal or cleanup catalog missing/corrupt | Enter recovery-only mode with an actionable diagnostic. Obtain verified state and reconcile/fence local backends; never initialize empty state automatically. |

Full-snapshot catch-up may skip committed revisions for definitions only. It
must preserve local cleanup obligations from the last applied catalog, and use
incarnations carried in the destination snapshot. It cannot discard an unfinished
cleanup catalog when a newer snapshot arrives. Application is serialized locally;
configuration synchronization does not itself admit resource votes or ownership.
Quorum-only members retain decisions, incarnations and applied metadata, but
have no local backend operations to perform.

Ordinary full-cluster restart recovers each node's monotonic agreement records
before seeking a matching majority and admitting resource activity. Revisions
alone cannot establish authority. Historical decision compaction must retain the
checkpoint's verified predecessor/decision evidence and the state needed for
unfinished requests and cleanup. Compaction and multi-revision catch-up are not
implemented by the current single-slot core.

Disk replacement or restoring stale backups is disaster recovery, not an ordinary
restart. A node with lost/rolled-back promises must remain excluded from votes
until an explicit recovery procedure establishes a safe new voting state. Do not
allow a former minority or stale restored majority to overwrite current history.
Automatic repair of lost agreement state and membership reconfiguration remain
unsupported; operators must retain a verified majority's durable state or stop
and fence the cluster before an explicit history reset. Backup/restore tooling
must capture journal and application catalogs together, never only resources.json.

## Client outcomes

Every mutation carries an idempotent request ID and expected base revision/digest.
Persist request identity and outcome as part of decision/status history before
acknowledging it. Retrying the same ID with different content is a conflict.
Recovering someone else's previously accepted value does not commit the waiting
client's edit; report that edit's own status or predecessor conflict.

New mutation/status endpoints use these results (existing runtime commands keep
their current exit codes):

| Result | Meaning | Exit status |
| --- | --- | --- |
| `applied` | Decision is durable and its application barrier completed. | 0 |
| `rejected` | Request was not admitted, or a durable final outcome establishes it did not commit. Include a reason such as validation, stale base, unsupported members or no quorum. | 1 |
| `pending` | Request is known; agreement or application has not finished. Include committed revision when known and any cleanup failure. | 2 |
| `unknown` | The responder cannot establish the request's outcome, including client timeout/disconnect. This is not an abort. | 3 |

Structured output includes request ID, result, committed revision/digest when
known, applied revision and an actionable reason. A known committed request with
failed cleanup stays `pending`, rather than becoming rejected. A timeout before
decision observation is `unknown`; query/retry the same request ID rather than
blindly submitting a second edit. Status retention/compaction must make expired
request IDs explicitly unknown, never falsely rejected or reusable within a
retained history.
