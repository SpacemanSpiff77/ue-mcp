import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const request = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v2.schema.json"), "utf8"));

describe("atomic Builder Stage C bounded multi-operation contract", () => {
  it("versions the material wire change and bounds one request to eight operations", () => {
    expect(request.$id).toBe("spacehead.blueprint-atomic-bridge.request@2.0");
    expect(request.$defs.contract.const).toBe("spacehead.blueprint-atomic-bridge@2.0");
    expect(request.$defs.buildPlan.properties.operations).toMatchObject({ minItems: 1, maxItems: 8 });
    expect(JSON.stringify(request.$defs.nodeReference)).toContain("logical_id");
  });

  it("uses one handler dispatch with explicit reverse restoration and save-last", () => {
    expect(handler.match(/ApplyAtomicBuildPlan\(/g)).toHaveLength(1);
    expect(handler).toContain("for (int32 Index = Applied.Num() - 1; Index >= 0; --Index)");
    expect(handler).toContain("CanonicalJsonObject(RollbackTopology) == CanonicalJsonObject(BaselineTopology)");
    expect(handler).toContain("CompileWithoutSave(Blueprint, CompileEvidence)");
    expect(handler).toContain("UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false)");
    expect(handler.indexOf("CompileWithoutSave(Blueprint, CompileEvidence)"))
      .toBeLessThan(handler.indexOf("UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false)"));
  });

  it("resolves new-node pins by unique semantic criteria and never inserts conversion nodes", () => {
    expect(handler).toContain("ResolveSemanticPin");
    expect(handler).toContain('TEXT("Condition"), EGPD_Input, TEXT("bool")');
    expect(handler).toContain('TEXT("then"), EGPD_Output, TEXT("exec")');
    expect(handler).not.toContain("CreateAutomaticConversionNodeAndConnections");
    expect(handler).not.toContain("CreatePromotedConnection");
  });

  it("exposes middle-operation and lost-response qualification checkpoints", () => {
    for (const checkpoint of ["AFTER_FIRST_MUTATION", "AFTER_MIDDLE_MUTATION", "AFTER_FINAL_MUTATION",
      "BEFORE_COMPILE", "VERIFICATION_FAILURE", "SAVE_FAILURE", "AFTER_SAVE_BEFORE_FINAL_RECEIPT",
      "AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT"])
      expect(handler).toContain(checkpoint);
    expect(handler).toContain('RollbackWholeBuild(TEXT("SAVE"), TEXT("FORCED_SAVE_FAILURE"), FailingOperation, false)');
    expect(handler).toContain('FaultCheckpoint == TEXT("AFTER_ROLLBACK_BEFORE_FINAL_RECEIPT") && Index == Operations->Num() - 1');
    expect(handler).toContain('ExpectedConnection->SetStringField(TEXT("from_pin_id"), FromSemanticPin->GetStringField(TEXT("pin_id")))');
    expect(handler).toContain('ExpectedConnection->SetStringField(TEXT("to_pin_id"), ToSemanticPin->GetStringField(TEXT("pin_id")))');
  });
});
