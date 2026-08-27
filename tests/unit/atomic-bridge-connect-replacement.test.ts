import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");

function section(start: string, end: string): string {
  const startIndex = handler.indexOf(start);
  const endIndex = handler.indexOf(end, startIndex);
  expect(startIndex).toBeGreaterThanOrEqual(0);
  expect(endIndex).toBeGreaterThan(startIndex);
  return handler.slice(startIndex, endIndex);
}

describe("atomic created-function direct connection replacement", () => {
  const accepted = section("bool IsNativeDirectConnectionMakingResponse", "TSharedPtr<FJsonObject> StructuralPinTypeEvidence");
  const mutation = section("TMap<FString, UEdGraphNode*> WholeLogicalNodes", "const int32 Middle = Operations->Num() / 2");

  it("admits exactly Unreal's native direct connection-making responses", () => {
    for (const response of [
      "CONNECT_RESPONSE_MAKE",
      "CONNECT_RESPONSE_BREAK_OTHERS_A",
      "CONNECT_RESPONSE_BREAK_OTHERS_B",
      "CONNECT_RESPONSE_BREAK_OTHERS_AB",
    ]) {
      expect(accepted).toContain(`case ${response}:`);
    }
    expect(accepted).toContain("return true;");
  });

  it("rejects incompatible, conversion, promotion, max, and unknown responses", () => {
    for (const response of [
      "CONNECT_RESPONSE_DISALLOW",
      "CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE",
      "CONNECT_RESPONSE_MAKE_WITH_PROMOTION",
      "CONNECT_RESPONSE_MAX",
    ]) {
      expect(accepted).not.toContain(`case ${response}:`);
    }
    expect(accepted).toMatch(/default:\s+return false;/);
  });

  it("delegates break-and-replace to Unreal and verifies one exact reciprocal link", () => {
    expect(mutation).toContain("IsNativeDirectConnectionMakingResponse(Response.Response.GetValue())");
    expect(mutation).toContain("TryCreateConnection(FromPin, ToPin)");
    expect(mutation).toContain("FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin)");
    expect(mutation).toContain("!bAlreadyConnected");
    expect(mutation).not.toContain("BreakAllPinLinks");
    expect(mutation).not.toContain("BreakSinglePinLink");
  });

  it("keeps deterministic failure and whole-graph rollback behavior", () => {
    expect(mutation).toContain('BodyFailureCode = TEXT("CONNECT_PINS_FAILED")');
    expect(handler).toContain("FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph, EGraphRemoveFlags::MarkTransient)");
  });
});
