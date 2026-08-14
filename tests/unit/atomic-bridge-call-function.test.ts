import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const topology = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp"), "utf8");
const schema = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v2.schema.json"), "utf8"));

describe("atomic Builder CallFunction.Standard", () => {
  it("advertises one additive exact capability and operation through the existing dispatch", () => {
    expect(handler).toContain('CallFunctionCapability = TEXT("graph.add-call-function-standard")');
    expect(handler).toContain('Version == TEXT("graph.add-call-function-standard@1.0")');
    expect(JSON.stringify(schema)).toContain("graph.add-call-function-standard@1.0");
    expect(handler.match(/ApplyAtomicBuildPlan\(/g)).toHaveLength(1);
  });

  it("preflights exact reflected function evidence before entering the mutation loop", () => {
    expect(handler).toContain("ResolveExactCallFunction");
    expect(handler).toContain("ExactFunctionParameters");
    expect(handler).toContain('FailureCode = TEXT("SIGNATURE_MISMATCH")');
    expect(handler).toContain('FailureCode = TEXT("METADATA_MISMATCH")');
    expect(handler.indexOf("TMap<FString, UFunction*> PlannedCallFunctions"))
      .toBeLessThan(handler.indexOf("for (int32 Index = 0; Index < Operations->Num(); ++Index)"));
  });

  it("binds the exact UFunction before Unreal authoritatively allocates pins", () => {
    const bind = handler.indexOf("Call->SetFromFunction(Function)");
    const postPlace = handler.indexOf("Call->PostPlacedNewNode()", bind);
    const allocate = handler.indexOf("Call->AllocateDefaultPins()", bind);
    expect(bind).toBeGreaterThan(0);
    expect(allocate).toBeGreaterThan(bind);
    expect(postPlace).toBeGreaterThan(allocate);
    expect(handler).toContain("ConstructedFunction && ConstructedFunction->GetName() == Function->GetName()");
    expect(handler).toContain("const int32 ExpectedPinCount = ExpectedParameters->Num() + (bPure ? 0 : 2) + 1");
    expect(handler).toContain("Call->Pins.Num() == ExpectedPinCount");
    expect(handler).toContain("bStatic && !Pin->bHidden");
    expect(handler).toContain('SetStringField(TEXT("failure_detail"), FailureDetail)');
    expect(handler).not.toContain("CreatePin(EGPD_");
  });

  it("verifies exact topology v2 delta, persistence, and restoration", () => {
    expect(handler).toContain("ExpectedPostV2");
    expect(handler).toContain("CanonicalExpectedPostV2");
    expect(handler).toContain("CanonicalJsonObject(PostTopologyV2) == CanonicalJsonObject(CanonicalExpectedPostV2)");
    expect(handler).toContain("CanonicalJsonObject(RollbackTopologyV2) == CanonicalJsonObject(PreTopologyV2)");
    expect(handler).toContain("CanonicalJsonObject(PersistedTopologyV2) == CanonicalJsonObject(PostTopologyV2)");
    expect(handler).toContain("PlannedCallFunctionGuids");
    expect(handler).toContain("AppendExpectedNode(ExpectedPost, PostTopology, CreatedGuid)");
    expect(handler).toContain("RefreshExpectedConnection(ExpectedPost, PostTopology, Action)");
    expect(handler).toContain("SemanticConnectionKey(A->AsObject()) < SemanticConnectionKey(B->AsObject())");
    expect(topology).toContain("authoritativeOwner");
    expect(topology).toContain("declaringOwner");
    expect(topology).toContain("callMode");
  });

  it("accepts only direct schema connections and reports conversion-required distinctly", () => {
    expect(handler).toContain("CompatibilityResponse.Response == CONNECT_RESPONSE_MAKE");
    expect(handler).toContain('TEXT("UNQUALIFIED_CONVERSION_REQUIRED")');
    expect(handler).not.toContain("CreateAutomaticConversionNodeAndConnections");
  });
});
