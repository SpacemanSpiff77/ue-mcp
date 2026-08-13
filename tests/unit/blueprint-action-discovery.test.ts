import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const source = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_Discovery.cpp"), "utf8");

describe("authoritative read-only Blueprint action discovery", () => {
  it("uses Unreal's action database, spawner, reflection, K2 conversion, and context filter APIs", () => {
    for (const api of [
      "FBlueprintActionDatabase::Get()", "GetAllActions()", "FBlueprintActionInfo",
      "GetAssociatedMemberField()", "GetAssociatedFunction()", "GetAssociatedProperty()",
      "GetSpawnerSignature()", "GetTemplateNode(", "ConvertPropertyToPinType",
      "FBlueprintActionFilter", "Filter.IsFiltered(Info)", "GetAuthoritativeClass()",
      "GetGuidFromClassByFieldName",
    ]) expect(source).toContain(api);
  });

  it("exposes a bounded paginated contract and persistent dirty-state evidence", () => {
    for (const evidence of [
      "spacehead.blueprint-action-discovery@1.0", "MaximumLimit = 5000", "MaximumContexts = 8",
      "raw_action_count", "matched_action_count", "has_more", "next_offset",
      "package_dirty_before", "package_dirty_after", "persistent_mutation_observed",
      "type_promotion_enabled", "enabled_plugins",
    ]) expect(source).toContain(evidence);
  });

  it("contains no authored graph mutation, compile, save, or package-write calls", () => {
    for (const forbidden of [
      "CompileBlueprint", "SaveAssetPackage", "SavePackage", "AddNode(", "CreateNewGuid(",
      "Modify()", "MarkBlueprintAsModified", "PostEditChange", "Invoke(",
    ]) expect(source).not.toContain(forbidden);
    expect(source).toContain("mutation_performed\"), false");
  });

  it("is registered as a Blueprint handler, surfaced by the tool, and fingerprint-bound", () => {
    const handlers = readFileSync(resolve(root,
      "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.cpp"), "utf8");
    const tools = readFileSync(resolve(root, "src/tools/blueprint.ts"), "utf8");
    const build = readFileSync(resolve(root,
      "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/UE_MCP_Bridge.Build.cs"), "utf8");
    expect(handlers).toContain("discover_blueprint_actions");
    expect(tools).toContain("discover_actions:");
    expect(build).toContain("BlueprintHandlers_Discovery.cpp");
  });
});
