import { describe, expect, it, vi } from "vitest";
import { readFile } from "node:fs/promises";
import { blueprintTool } from "../../src/tools/blueprint.js";
import type { ToolContext } from "../../src/types.js";

describe("blueprint.read_function_topology", () => {
  it("maps only the exact bounded read parameters to the native provider", async () => {
    const call = vi.fn(async () => ({ contractVersion: "spacehead.selected-function-topology@1.0" }));
    const ctx = { bridge: { call }, project: {} } as unknown as ToolContext;
    const result = await blueprintTool.handler(ctx, {
      action: "read_function_topology",
      assetPath: "/Game/Landscape/Voxel/Components/BPC_VoxelHealingState",
      functionName: "IsHealingBlockedByStructure",
      maxNodes: 64,
      maxPins: 512,
      maxConnections: 1024,
      graphName: "must-not-be-used",
    });

    expect(result).toEqual({ contractVersion: "spacehead.selected-function-topology@1.0" });
    expect(call).toHaveBeenCalledOnce();
    expect(call).toHaveBeenCalledWith("read_blueprint_function_topology", {
      path: "/Game/Landscape/Voxel/Components/BPC_VoxelHealingState",
      functionName: "IsHealingBlockedByStructure",
      maxNodes: 64,
      maxPins: 512,
      maxConnections: 1024,
    }, 180_000);
  });

  it("registers a native read-only handler and contains no mutation calls", async () => {
    const root = process.cwd();
    const registration = await readFile(
      `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.cpp`,
      "utf8",
    );
    const selectedProvider = await readFile(
      `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_Topology.cpp`,
      "utf8",
    );
    const sharedSerializer = await readFile(
      `${root}/plugin/UE_MCP_Bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp`,
      "utf8",
    );
    const provider = `${selectedProvider}\n${sharedSerializer}`;

    expect(registration).toContain('TEXT("read_blueprint_function_topology")');
    expect(provider).toContain('TEXT("spacehead.selected-function-topology@1.0")');
    expect(provider).toContain("Blueprint->FunctionGraphs");
    expect(provider).toContain("ESearchCase::CaseSensitive");
    expect(provider).toContain("Pin->LinkedTo");
    expect(provider).toContain("Pin->PinId");
    expect(provider).toContain("PinSubCategoryMemberReference");
    expect(provider).toContain("bSerializeAsSinglePrecisionFloat");
    expect(provider).toContain("Node->NodeGuid");
    expect(provider).toContain("TargetGraph->GraphGuid");
    expect(provider).toContain("IsDirty()");
    expect(provider).toContain('TEXT("function-missing")');
    expect(provider).toContain('TEXT("function-ambiguous")');
    expect(provider).toContain('TEXT("truncated")');
    expect(provider).toContain('TEXT("sourceNodeId")');
    expect(provider).toContain('TEXT("sourcePinId")');
    expect(provider).toContain('TEXT("targetNodeId")');
    expect(provider).toContain('TEXT("targetPinId")');
    expect(provider).toContain('TEXT("unresolvedEndpoints")');
    expect(provider).toContain('TEXT("allOrNothing")');
    for (const forbidden of [
      "CompileBlueprint", "SavePackage", "MarkPackageDirty", "ReconstructNode",
      "Modify()", "CreateNewGuid", "AddNode(", "RemoveNode(",
    ]) {
      expect(provider).not.toContain(forbidden);
    }
  });
});
