import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const serializer = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp"), "utf8");

describe("atomic Blueprint semantic topology v2", () => {
  it("keeps the qualified v1 projection and introduces an explicit independent v2 contract", () => {
    expect(handler).toContain('TopologyVersion = TEXT("spacehead.blueprint-semantic-topology@1.0")');
    expect(handler).toContain('TopologyV2Version = TEXT("spacehead.blueprint-semantic-topology@2.0")');
    expect(handler).toContain("TSharedPtr<FJsonObject> SemanticTopology(");
    expect(handler).toContain("TSharedPtr<FJsonObject> SemanticTopologyV2(");
    expect(handler).toContain("Version != TopologyVersion && Version != TopologyV2Version");
  });

  it("projects exact function/member identity and rich Unreal pin type evidence", () => {
    for (const field of [
      "function_reference", "member_reference", "call_mode", "self_context", "guid", "owner", "name",
      "subcategory", "subcategory_object", "subcategory_member_reference", "container", "value_terminal_type",
      "is_reference", "is_const", "is_weak_reference", "is_uobject_wrapper",
      "serialize_as_single_precision_float", "default_value", "default_object", "default_text",
    ]) expect(handler).toContain(`TEXT("${field}")`);
    for (const rawField of [
      "subCategoryMemberReference", "valueTerminalType", "isReference", "isConst", "isWeakPointer",
      "isUObjectWrapper", "serializeAsSinglePrecisionFloat", "defaultValue", "defaultObject", "defaultTextValue",
    ]) expect(serializer).toContain(`TEXT("${rawField}")`);
  });

  it("uses v2 as additive exact persistence evidence while v1 remains the mutation delta contract", () => {
    expect(handler).toContain('TEXT("semantic_topology_v2_version")');
    expect(handler).toContain('TEXT("pre_semantic_fingerprint_v2")');
    expect(handler).toContain('TEXT("post_semantic_fingerprint_v2")');
    expect(handler.match(/TEXT\("persisted_semantic_fingerprint_v2"\)/g)).toHaveLength(3);
    expect(handler.match(/CanonicalJsonObject\(PersistedTopologyV2\) == CanonicalJsonObject\(PostTopologyV2\)/g))
      .toHaveLength(3);
    expect(handler).toContain("CanonicalJsonObject(PostTopology) == CanonicalJsonObject(CanonicalExpectedPost)");
    expect(handler).toContain("CanonicalJsonObject(PostTopology) == CanonicalJsonObject(ExpectedPost)");
  });
});
