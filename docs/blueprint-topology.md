# Full Blueprint Topology

`blueprint.read_blueprint_topology` is a native, read-only provider for a
complete local graph inventory and exact supported topology for one Blueprint.
It observes Unreal once on the game thread and produces the logical contract
`spacehead.full-blueprint-topology@1.0`; it does not compose `list_graphs`,
`read_graph`, or repeated live graph reads.

## Inventory

The provider pointer-deduplicates graphs while retaining every collection
membership as evidence. It inspects:

- `FunctionGraphs`, `UbergraphPages`, `MacroGraphs`, and
  `DelegateSignatureGraphs`
- `ImplementedInterfaces[].Graphs`
- every `UBlueprintExtension::GetAllGraphs()` result
- recursive `SubGraphs`
- the User Construction Script
- `IntermediateGeneratedGraphs` and compiler `EventGraphs`
- `UBlueprint::GetAllGraphs()`
- a recursive `UEdGraph` ownership audit below the target Blueprint

Graphs not owned through the target Blueprint are not treated as locally
authored. This excludes inherited parent-Blueprint graphs. Unknown owned
graphs remain explicit `unclassified-owned` inventory records.

The normalized classifications are function, Event Graph, Construction
Script, macro, interface implementation, nested authored graph,
extension-authored graph, delegate signature, generated, transient,
unsupported authored, and unclassified owned. Qualified K2 authored graphs
use `capture-topology`; signature, generated, and transient graphs remain
explicit inventory-only records. Authored custom schemas are reported as
unsupported and prevent topology completeness.

## Exact topology and completeness

Captured graphs use the same qualified serializer as
`blueprint.read_function_topology`. It emits stable graph, node, and
node-scoped pin identities; full pin types and defaults; semantic references;
exact execution/data endpoints; and explicit unresolved endpoints.

`graphInventoryComplete` means every configured UE 5.8 source was inspected,
the pointer-deduplicated union was classified, and the unpaginated inventory
was returned. `topologyComplete` additionally requires:

- the captured graph set and all graph/node/pin/connection counts reconcile
- no unsupported authored or unclassified owned graphs
- no unresolved endpoints or duplicate pin identity within one node
- no bound omission or truncation
- no package dirty-state change
- no compile, save, reconstruction, or mutation operation

Event Graphs do not require function entry/result nodes. Pinless comment and
presentation nodes are valid. Delegate signature graphs are inventory-only.

## Bounds and omission

Defaults are 64 authored graphs, 512 nodes/4,096 pins/8,192 connections per
graph, 4,096 nodes/32,768 pins/65,536 connections across captured graphs, and
a 3.5 MiB inline serialized payload boundary. Safe hard maxima are returned in
`limits`.

Graph and topology-count bounds are all-or-nothing. If one is exceeded, the
provider returns the complete inventory, totals, limits, and omission
violations with an empty `graphs` array, `truncated: true`, `dataOmitted: true`,
and `allOrNothing: true`.

Serialized size is a transport boundary, not a topology qualification bound.
Qualified results at or below the inline boundary are returned normally.
Larger results are frozen as canonical UTF-8 bytes during that same Unreal
observation and the first call returns
`spacehead.full-blueprint-topology-multipart@1.0`. The manifest contains an
opaque capture handle, total length, 2 MiB raw chunk bound, chunk count,
inventory evidence, expiry, and SHA-256 `snapshotHash`.

`blueprint.read_blueprint_topology_chunk` returns one deterministic base64
chunk from frozen memory with its exact index, offset, raw length, SHA-256, and
manifest association. It never re-reads Unreal. Captures expire after five
minutes and are also bounded by active-capture and retained-byte quotas.
`blueprint.release_blueprint_topology_capture` releases a capture explicitly.
Wrong, expired, or released handles and invalid indexes fail explicitly.

## Current support boundary

Exact topology is currently qualified for authored standard K2-schema graphs. Animation
Blueprint state machines, AnimGraphs, Control Rig/RigVM graphs, material
graphs, and other custom authored schemas are inventoried as unsupported
rather than silently serialized with K2 assumptions. Supporting those schemas
requires schema-specific serializers and completeness rules in later
contracts.
