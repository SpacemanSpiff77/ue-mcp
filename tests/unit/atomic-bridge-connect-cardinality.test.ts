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

describe("atomic bridge CONNECT_PINS cardinality", () => {
  it("allows one data output to feed two compatible data inputs", () => {
    const preflight = section("TSet<FString> ClaimedFutureDataInputs", "TMap<FString, UEdGraphNode*> WholeLogicalNodes");
    expect(preflight).toContain('FromCategory != TEXT("exec") || !ClaimedFutureExecutionOutputs.Contains(FromQualifiedPin)');
    expect(preflight).toContain('ToCategory == TEXT("exec") || !ClaimedFutureDataInputs.Contains(ToQualifiedPin)');
    expect(preflight).toContain('if (ToCategory != TEXT("exec")) ClaimedFutureDataInputs.Add(ToQualifiedPin)');
    expect(preflight).not.toContain("ClaimedFutureDataOutputs");
  });

  it("rejects a second source for a data input and an exact duplicate", () => {
    const preflight = section("TSet<FString> ClaimedFutureDataInputs", "TMap<FString, UEdGraphNode*> WholeLogicalNodes");
    expect(preflight).toContain("!ClaimedFutureConnections.Contains(ConnectionKey)");
    expect(preflight).toContain("ClaimedFutureConnections.Add(ConnectionKey)");
    expect(preflight).toContain("!ClaimedFutureDataInputs.Contains(ToQualifiedPin)");
    const mutation = section("TMap<FString, UEdGraphNode*> WholeLogicalNodes", "const int32 Middle = Operations->Num() / 2");
    expect(mutation).toContain("!bAlreadyConnected");
  });

  it("delegates runtime type and conflict cardinality to Unreal without conversion or replacement", () => {
    const wholeMutation = section("TMap<FString, UEdGraphNode*> WholeLogicalNodes", "const int32 Middle = Operations->Num() / 2");
    const stageC = section('else if ((Version == TEXT("graph.connect-pins@1.0") || Version == TEXT("graph.disconnect-pins@1.0"))',
      "ConnectionDecision = ConnectionDecisionEvidence");
    for (const mutation of [wholeMutation, stageC]) {
      expect(mutation).toContain("CONNECT_RESPONSE_MAKE");
      expect(mutation).toContain("TryCreateConnection(FromPin, ToPin)");
      expect(mutation).not.toContain("CreateAutomaticConversionNodeAndConnections");
      expect(mutation).not.toContain("CreatePromotedConnection");
    }
    expect(stageC).toContain("!bConnected");
    expect(stageC).not.toContain("FromPin->LinkedTo.IsEmpty() && ToPin->LinkedTo.IsEmpty()");
  });

  it("keeps exec output single-target while allowing exec input fan-in", () => {
    const preflight = section("TSet<FString> ClaimedFutureDataInputs", "TMap<FString, UEdGraphNode*> WholeLogicalNodes");
    expect(preflight).toContain('FromCategory != TEXT("exec") || !ClaimedFutureExecutionOutputs.Contains(FromQualifiedPin)');
    expect(preflight).toContain('if (FromCategory == TEXT("exec")) ClaimedFutureExecutionOutputs.Add(FromQualifiedPin)');
    expect(preflight).toContain('ToCategory == TEXT("exec") || !ClaimedFutureDataInputs.Contains(ToQualifiedPin)');

    const stageB = section("if (StageBOperation == EStageBOperation::ConnectPins)", "const TSharedPtr<FJsonObject> ExpectedConnection");
    expect(stageB).toContain("bExactExecutionPins");
    expect(stageB).toContain("FromPin->LinkedTo.IsEmpty() && ToPin->LinkedTo.IsEmpty()");
  });

  it("keeps invalid directions, same-node links, incompatible categories, disconnect, and decision evidence intact", () => {
    expect(handler).toContain("FromPin->Direction == EGPD_Output && ToPin->Direction == EGPD_Input");
    expect(handler).toContain("FromNode != ToNode");
    expect(handler).toContain("FromCategory.Equals(ToCategory, ESearchCase::IgnoreCase)");
    expect(handler).toContain("Schema->BreakSinglePinLink(FromPin, ToPin)");
    expect(handler).toContain("bFromCardinalityClean");
    expect(handler).toContain("bToCardinalityClean");
    expect(handler).toContain("ConnectionDecisionEvidence(OperationId, Schema, CompatibilityResponse");
  });
});
