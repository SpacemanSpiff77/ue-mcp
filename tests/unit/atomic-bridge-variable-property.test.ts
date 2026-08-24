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

describe("atomic Builder Variable/Property Pass 1", () => {
  it("advertises additive exact GET and SET operations through the existing dispatch", () => {
    expect(handler).toContain('VariableGetCapability = TEXT("graph.add-variable-get")');
    expect(handler).toContain('VariableSetCapability = TEXT("graph.add-variable-set")');
    expect(handler).toContain('FirstOperationVersion == TEXT("graph.add-variable-get@1.0")');
    expect(handler).toContain('FirstOperationVersion == TEXT("graph.add-variable-set@1.0")');
    expect(JSON.stringify(schema)).toContain("graph.add-variable-get@1.0");
    expect(JSON.stringify(schema)).toContain("graph.add-variable-set@1.0");
    expect(handler.match(/ApplyAtomicBuildPlan\(/g)).toHaveLength(1);
  });

  it("resolves one exact admitted native FProperty before mutation", () => {
    expect(handler).toContain("ResolveExactVariableProperty");
    expect(handler).toContain("FindFProperty<FProperty>(OwnerClass, *Member)");
    expect(handler).toContain("Property->GetPathName() != AssociatedFieldPath");
    expect(handler).toContain("CPF_BlueprintVisible");
    expect(handler).not.toContain("Property->HasAnyPropertyFlags(CPF_BlueprintReadOnly)");
    expect(handler.indexOf("TMap<FString, FProperty*> PlannedVariableProperties"))
      .toBeLessThan(handler.indexOf("for (int32 Index = 0; Index < Operations->Num(); ++Index)"));
  });

  it("lets Unreal allocate authoritative VariableGet/VariableSet pins and verifies exact identity", () => {
    expect(handler).toContain("NewObject<UK2Node_VariableGet>(Graph)");
    expect(handler).toContain("NewObject<UK2Node_VariableSet>(Graph)");
    expect(handler).toContain("Variable->VariableReference.SetFromField<FProperty>(Property, bSelfContext)");
    expect(handler).toContain("Variable->AllocateDefaultPins()");
    expect(handler).not.toContain("CreatePin(EGPD_");
    expect(handler).toContain("PlannedVariableGuids");
    expect(handler).toContain("PlannedLogicalVariableGets");
    expect(handler).toContain("UK2Node_Variable* Variable = Cast<UK2Node_Variable>(Pair.Value)");
    expect(topology).toContain("ExactVariableOwner");
    expect(topology).toContain('SetStringField(TEXT("memberGuid")');
    expect(topology).toContain('SetBoolField(TEXT("selfContext")');
  });

  it("reuses the same property mechanism for external targets and Blueprint-defined members", () => {
    expect(handler).toContain('TryGetBoolField(TEXT("self_context"), bSelfContext)');
    expect(handler).toContain("PlannedLogicalVariableSelfContexts");
    expect(handler).toContain("UBlueprint::GetGuidFromClassByFieldName<FProperty>");
    expect(handler).toContain('FailureCode = TEXT("BLUEPRINT_PROPERTY_IDENTITY_MISMATCH")');
    expect(handler).not.toContain('FailureCode = TEXT("BLUEPRINT_PROPERTY_DEFERRED")');
    expect(handler).toContain("Variable->VariableReference.IsSelfContext() == bSelfContext");
  });
});
