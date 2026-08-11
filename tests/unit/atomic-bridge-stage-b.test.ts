import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const request = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v2.schema.json"), "utf8"));
const receipt = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/receipt-v2.schema.json"), "utf8"));

describe("atomic Builder Stage B minimum graph vocabulary", () => {
  it("advertises each narrow capability through the existing atomic raw method", () => {
    for (const capability of ["graph.add-node", "graph.connect-pins", "graph.disconnect-pins", "graph.set-pin-default"])
      expect(handler).toContain(`TEXT("${capability}")`);
    expect(handler).toContain('const TCHAR* RawMethodIdentity = TEXT("apply_atomic_build_plan")');
  });

  it("uses direct deterministic graph/schema APIs with no conversion or promotion behavior", () => {
    expect(handler).toContain("NewObject<UK2Node_IfThenElse>(Graph)");
    expect(handler).toContain("Branch->NodeGuid = ParsedCreatedGuid");
    expect(handler).toContain("Schema->CanCreateConnection(FromPin, ToPin)");
    expect(handler).toContain("Response.Response == CONNECT_RESPONSE_MAKE");
    expect(handler).toContain("Schema->TryCreateConnection(FromPin, ToPin)");
    expect(handler).toContain("Schema->BreakSinglePinLink(FromPin, ToPin)");
    expect(handler).not.toContain("CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE");
    expect(handler).not.toContain("CONNECT_RESPONSE_MAKE_WITH_PROMOTION");
  });

  it("restores add/connect/disconnect explicitly and proves the complete canonical baseline", () => {
    expect(handler).toContain("Graph->RemoveNode(ExistingAdded)");
    expect(handler).toContain("CompileWithoutSave(Blueprint, RollbackCompile)");
    expect(handler).toContain("CanonicalJsonObject(RollbackTopology) == CanonicalJsonObject(BaselineTopology)");
    expect(handler).toContain("Package->SetDirtyFlag(false)");
    expect(handler).toContain("RESTORATION_UNPROVEN");
  });

  it("versions all four request operations and deterministic receipt evidence", () => {
    const encodedRequest = JSON.stringify(request.$defs.buildSpecOperation);
    for (const operation of ["graph.add-node@1.0", "graph.connect-pins@1.0", "graph.disconnect-pins@1.0", "graph.set-pin-default@1.0"])
      expect(encodedRequest).toContain(operation);
    expect(receipt.$defs.evidence.properties).toMatchObject({
      operation_count: { type: "integer", minimum: 1, maximum: 8 },
      ordered_operation_ids: { type: "array", maxItems: 8 },
      per_operation_results: { type: "array", maxItems: 8 },
    });
  });
});
