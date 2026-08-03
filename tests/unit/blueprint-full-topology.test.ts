import { describe, expect, it, vi } from "vitest";
import { readFile } from "node:fs/promises";
import { blueprintTool } from "../../src/tools/blueprint.js";
import { ALL_TOOLS } from "../../src/tools.js";
import { buildDefaults } from "../../src/flow/loader.js";
import { buildFlowRegistry } from "../../src/flow/registry.js";
import type { ToolContext } from "../../src/types.js";

const root = process.cwd();
const fullProviderPath =
  `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_FullTopology.cpp`;
const serializerPath =
  `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp`;

describe("blueprint.read_blueprint_topology", () => {
  it("maps only the public bounded read parameters to one native call", async () => {
    const call = vi.fn(async () => ({ contractVersion: "spacehead.full-blueprint-topology@1.0" }));
    const ctx = { bridge: { call }, project: {} } as unknown as ToolContext;
    const result = await blueprintTool.handler(ctx, {
      action: "read_blueprint_topology",
      assetPath: "/Game/Test/BP_Full",
      maxAuthoredGraphs: 64,
      maxNodesPerGraph: 512,
      maxPinsPerGraph: 4096,
      maxConnectionsPerGraph: 8192,
      maxTotalNodes: 4096,
      maxTotalPins: 32768,
      maxTotalConnections: 65536,
      maxSerializedBytes: 3670016,
      graphName: "must-not-be-used",
      offset: 123,
    });

    expect(result).toEqual({ contractVersion: "spacehead.full-blueprint-topology@1.0" });
    expect(call).toHaveBeenCalledOnce();
    expect(call).toHaveBeenCalledWith("read_blueprint_topology", {
      path: "/Game/Test/BP_Full",
      maxAuthoredGraphs: 64,
      maxNodesPerGraph: 512,
      maxPinsPerGraph: 4096,
      maxConnectionsPerGraph: 8192,
      maxTotalNodes: 4096,
      maxTotalPins: 32768,
      maxTotalConnections: 65536,
      maxSerializedBytes: 3670016,
    }, 180_000);
  });

  it("resolves from runtime Flow defaults and registry without filesystem fallback", async () => {
    const defaults = buildDefaults(ALL_TOOLS) as {
      tasks: Record<string, { class_path: string }>;
    };
    expect(defaults.tasks["blueprint.read_blueprint_topology"]).toMatchObject({
      class_path: "blueprint.read_blueprint_topology",
    });

    const call = vi.fn(async () => ({ status: "exact" }));
    const registry = buildFlowRegistry(ALL_TOOLS);
    expect(registry.listRegistered()).toContain("blueprint.read_blueprint_topology");
    const task = await registry.create(
      "blueprint.read_blueprint_topology",
      { bridge: { call }, project: {}, registry },
      { assetPath: "/Game/Test/BP_Full" },
    );
    const result = await task.run();
    expect(result.success).toBe(true);
    expect(call).toHaveBeenCalledWith("read_blueprint_topology", {
      path: "/Game/Test/BP_Full",
      maxAuthoredGraphs: undefined,
      maxNodesPerGraph: undefined,
      maxPinsPerGraph: undefined,
      maxConnectionsPerGraph: undefined,
      maxTotalNodes: undefined,
      maxTotalPins: undefined,
      maxTotalConnections: undefined,
      maxSerializedBytes: undefined,
    }, 180_000);
  });

  it("registers the native handler and full provider contract", async () => {
    const registration = await readFile(
      `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.cpp`,
      "utf8",
    );
    const provider = await readFile(fullProviderPath, "utf8");
    expect(registration).toContain('TEXT("read_blueprint_topology")');
    expect(provider).toContain('TEXT("spacehead.full-blueprint-topology@1.0")');
    expect(provider).toContain('TEXT("full-blueprint")');
    expect(provider).toContain("UE_MCP_BlueprintTopology::SerializeGraph");
  });

  it("inventories every required UE 5.8 source and retains duplicate membership evidence", async () => {
    const provider = await readFile(fullProviderPath, "utf8");
    for (const source of [
      "FunctionGraphs", "UbergraphPages", "MacroGraphs", "DelegateSignatureGraphs",
      "ImplementedInterfaces", "GetExtensions()", "RecursiveSubGraphs",
      "FindUserConstructionScript", "IntermediateGeneratedGraphs", "EventGraphs",
      "GetAllGraphs", "GetObjectsWithOuter", "OwnedUEdGraphAudit",
    ]) {
      expect(provider).toContain(source);
    }
    expect(provider).toContain("DuplicateMembershipCount");
    expect(provider).toContain("ForeignGraphReferences");
    expect(provider).toContain("inheritedGraphExclusionCount");
  });

  it("classifies function, Event Graph, Construction Script, macro, signature, interface, nested, generated, transient, unsupported, and unclassified graphs", async () => {
    const provider = await readFile(fullProviderPath, "utf8");
    for (const classification of [
      "function", "event-graph", "construction-script", "macro",
      "delegate-signature", "interface-implementation", "nested-authored",
      "generated", "transient", "unsupported-authored", "unclassified-owned",
    ]) {
      expect(provider).toContain(`TEXT("${classification}")`);
    }
    expect(provider).toContain("authored graph schema is not supported");
    expect(provider).toContain("absent from every known UE 5.8 Blueprint graph collection");
  });

  it("uses the shared qualified serializer and allows legitimate pinless nodes", async () => {
    const selected = await readFile(
      `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_Topology.cpp`,
      "utf8",
    );
    const serializer = await readFile(serializerPath, "utf8");
    expect(selected).toContain("UE_MCP_BlueprintTopology::SerializeGraph");
    expect(serializer).toContain("Node->Pins");
    expect(serializer).not.toMatch(/Pins\.Num\(\)\s*==\s*0[\s\S]{0,120}(fail|error|incomplete)/i);
    expect(serializer).toContain('TEXT("semanticRole")');
    expect(serializer).toContain('TEXT("semanticIdentity")');
  });

  it("keeps pin identity node-scoped and rejects duplicates only inside one node", async () => {
    const serializer = await readFile(serializerPath, "utf8");
    expect(serializer).toContain('TEXT("pinIdentityScope"), TEXT("node")');
    expect(serializer).toContain("TSet<FString> NodeScopedPinIds");
    expect(serializer).toContain("NodeScopedPinIds.Contains");
    expect(serializer).toContain("DuplicatePinIdentityCount");
    expect(serializer).not.toContain("TSet<FString> GlobalPinIds");
  });

  it("serializes exact execution/data links and explicit unresolved endpoints deterministically", async () => {
    const serializer = await readFile(serializerPath, "utf8");
    for (const field of [
      "sourceNodeId", "sourcePinId", "targetNodeId", "targetPinId",
      "classification", "unresolvedEndpoints", "defaultValue", "defaultObject",
      "defaultTextValue", "typeInfo", "calledFunction", "variable", "macro",
    ]) {
      expect(serializer).toContain(`TEXT("${field}")`);
    }
    expect(serializer).toContain("ConnectionRecords.Sort");
    expect(serializer).toContain("UnresolvedRecords.Sort");
    expect(serializer).toContain("Nodes.Sort");
    expect(serializer).toContain("Pins.Sort");
  });

  it("builds a stable observation-free inventory hash and token", async () => {
    const provider = await readFile(fullProviderPath, "utf8");
    expect(provider).toContain("CanonicalInventory");
    expect(provider).toContain("Sha1String(CanonicalInventory)");
    expect(provider).toContain('TEXT("inventoryToken"), TEXT("sha1:")');
    expect(provider).not.toMatch(/Set(?:String|Number)Field\(TEXT\("(timestamp|observedAt|readAt)"\)/);
  });

  it("preflights per-graph, whole-Blueprint, authored-count, and payload bounds with atomic omission", async () => {
    const provider = await readFile(fullProviderPath, "utf8");
    for (const bound of [
      "MaxAuthoredGraphs", "MaxNodesPerGraph", "MaxPinsPerGraph",
      "MaxConnectionsPerGraph", "MaxTotalNodes", "MaxTotalPins",
      "MaxTotalConnections", "MaxSerializedBytes",
    ]) {
      expect(provider).toContain(bound);
    }
    expect(provider).toContain('SetArrayField(TEXT("graphs"), {})');
    expect(provider).toContain('SetBoolField(TEXT("truncated"), true)');
    expect(provider).toContain('SetBoolField(TEXT("dataOmitted"), true)');
    expect(provider).toContain('SetBoolField(TEXT("allOrNothing"), true)');
  });

  it("enforces exact completeness and a read-only dirty-state mutation guard", async () => {
    const provider = await readFile(fullProviderPath, "utf8");
    expect(provider).toContain("bGraphInventoryComplete");
    expect(provider).toContain("bCapturedSetReconciled");
    expect(provider).toContain("bCountReconciled");
    expect(provider).toContain("UnsupportedGraphCount == 0");
    expect(provider).toContain("UnclassifiedGraphCount == 0");
    expect(provider).toContain("UnresolvedEndpointCount == 0");
    expect(provider).toContain("DuplicatePinIdentityCount == 0");
    expect(provider).toContain("IsDirty()");
    expect(provider).toContain('TEXT("mutationGuardPassed")');
    for (const forbidden of [
      "CompileBlueprint", "SavePackage", "MarkPackageDirty", "ReconstructNode",
      "Modify()", "CreateNewGuid", "AddNode(", "RemoveNode(",
    ]) {
      expect(provider).not.toContain(forbidden);
    }
  });
});
