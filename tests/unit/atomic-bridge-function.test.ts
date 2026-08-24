import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";

const source = readFileSync("plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp", "utf8");
const schema = readFileSync("plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v2.schema.json", "utf8");

describe("atomic Blueprint function-shell construction", () => {
  it("advertises one additive asset-level function capability", () => {
    expect(source).toContain('FunctionAddCapability = TEXT("function.add")');
    expect(source).toContain('FirstOperationVersion == TEXT("function.add@1.0")');
    expect(schema).toContain('"function.add@1.0"');
    expect(schema).toContain('"function.add"');
  });

  it("advertises and preflights future-function body composition without runtime graph identity", () => {
    expect(source).toContain('WholeFunctionComposeCapability = TEXT("function.compose-created-body")');
    expect(source).toContain('Kind == TEXT("function_entry")');
    expect(source).toContain('Kind == TEXT("function_return")');
    expect(source).toContain('GraphKind == TEXT("created_function")');
    expect(source).toContain('SpecKindValue == TEXT("add_node")');
    expect(source).toContain('WholeFunctionPreflightFailure = Function ? TEXT("CALLFUNCTION_BODY_MISMATCH") : FunctionFailure');
    expect(source).toContain('TEXT("FUTURE_FUNCTION_BODY_INVALID")');
    expect(schema).toContain('"createdFunctionGraphReference"');
    expect(schema).toContain('"function_entry", "function_return"');
  });

  it("uses the authoritative UE 5.8 function graph and terminal APIs", () => {
    expect(source).toContain("FKismetNameValidator(Blueprint).IsValid(FunctionName) == EValidatorResult::Ok");
    expect(source).toContain("FBlueprintEditorUtils::CreateNewGraph");
    expect(source).toContain("FBlueprintEditorUtils::AddFunctionGraph<UFunction>");
    expect(source).toContain("Entry->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Output, false)");
    expect(source).toContain("FBlueprintEditorUtils::FindOrCreateFunctionResultNode(Entry)");
    expect(source).toContain("ResultNode->CreateUserDefinedPin(FName(*Name), PinType, EGPD_Input, false)");
  });

  it("keeps compile, verification, save-last, and rollback removal inside one dispatch", () => {
    expect(source).toContain("ExactFunctionShell(NewGraph, Inputs, Outputs)");
    expect(source).toContain("CompileWithoutSave(Blueprint, CompileEvidence)");
    expect(source).toContain("FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph, EGraphRemoveFlags::MarkTransient)");
    expect(source).toContain("UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false)");
    expect(source.indexOf("ExactFunctionShell(NewGraph, Inputs, Outputs)")).toBeLessThan(
      source.lastIndexOf("UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false)"));
  });

  it("creates qualified body nodes and connections before the single compile and exact verification", () => {
    expect(source).toContain("TMap<FString, UEdGraphNode*> WholeLogicalNodes");
    expect(source).toContain("UK2Node_IfThenElse* Branch = NewObject<UK2Node_IfThenElse>(NewGraph)");
    expect(source).toContain("UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(NewGraph)");
    expect(source).toContain("TryCreateConnection(FromPin, ToPin)");
    expect(source).toContain('Verification->SetBoolField(TEXT("exact_body_nodes"), bBodyNodesExact)');
    expect(source).toContain('Verification->SetBoolField(TEXT("exact_connection_set"), bBodyConnectionsExact)');
    expect(source).toContain('TEXT("FORCED_FAILURE_AFTER_BODY_MUTATION")');
    expect(source).toContain('PackagePath == TEXT("/Game/Tests/Builder/BP_PhaseE_WholeFunction")');
    expect(source).toContain('Checkpoint == TEXT("AFTER_FINAL_MUTATION")');
    expect(source.indexOf("TMap<FString, UEdGraphNode*> WholeLogicalNodes")).toBeLessThan(
      source.indexOf("CompileWithoutSave(Blueprint, CompileEvidence)", source.indexOf("TMap<FString, UEdGraphNode*> WholeLogicalNodes")));
  });
});
