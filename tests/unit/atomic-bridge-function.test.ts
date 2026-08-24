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
});
