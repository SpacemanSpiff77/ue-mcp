import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(__dirname, "../..");
const buildSpec = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const registration = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers.cpp"), "utf8");
const qualification = buildSpec.slice(buildSpec.indexOf("FBlueprintHandlers::QualifyCallFunctionCandidates"));

describe("CallFunction Pass 4 qualification bridge", () => {
  it("registers one bounded long-running qualification handler", () => {
    expect(registration).toContain("RegisterHandlerWithTimeout(TEXT(\"qualify_call_function_candidates\")");
    expect(qualification).toContain("Candidates->Num() > 500");
    expect(qualification).toContain("/Game/Tests/Builder/");
    expect(qualification).toContain("spacehead.call-function-qualification-harness@1.0");
    expect(qualification).toContain('DeclaringOwner.Contains(TEXT(".SKEL_")) && BlueprintMemberGuid.IsEmpty()');
  });

  it("constructs ordinary native nodes, compiles without save, verifies topology v2, and restores the fixture", () => {
    expect(qualification).toContain("NewObject<UK2Node_CallFunction>");
    expect(qualification).toContain("Call->GetClass() == UK2Node_CallFunction::StaticClass()");
    expect(qualification).toContain("CompileWithoutSave(Blueprint");
    expect(qualification).toContain("SemanticTopologyV2(Graph");
    expect(qualification).toContain("Graph->RemoveNode(Call)");
    expect(qualification).toContain("CanonicalJsonObject(Cleanup) == BaselineCanonical");
    expect(qualification).toContain("save_performed");
    expect(qualification).not.toContain("SaveLoadedAsset");
  });

  it("validates qualified authored defaults with exact types, fixed enum identity, and common structs", () => {
    const codec = buildSpec.slice(buildSpec.indexOf("bool QualifiedDefaultMatchesType"),
      buildSpec.indexOf("bool ExactFunctionParameters"));
    for (const name of ["bool", "byte", "int", "int64", "float", "double", "name", "string", "enum",
      "vector", "rotator"])
      expect(codec).toContain(`Codec == TEXT(\"${name}\")`);
    expect(codec).toContain("ExactExpectedPinType");
    expect(codec).toContain("EnumType == Enum->GetPathName()");
    expect(codec).toContain("EGetByNameFlags::CaseSensitive");
    expect(codec).toContain("TBaseStructure<FVector>::Get()");
    expect(codec).toContain("TBaseStructure<FRotator>::Get()");
    expect(codec).not.toContain('Codec == TEXT("text")');
  });

  it("rejects invalid semantic defaults in live preflight before the mutation loop", () => {
    const preflight = buildSpec.indexOf("PlannedLogicalCallFunctions");
    const mutation = buildSpec.indexOf("enum class EAppliedKind", preflight);
    expect(preflight).toBeGreaterThan(0);
    expect(mutation).toBeGreaterThan(preflight);
    const gate = buildSpec.slice(preflight, mutation);
    expect(gate).toContain("QualifiedDefaultMatchesType");
    expect(gate).toContain("ENUM_MEMBER_MISSING");
    expect(gate).toContain("FAILED_PRE_MUTATION");
  });
});
