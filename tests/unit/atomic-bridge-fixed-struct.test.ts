import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const read = (path: string) => readFileSync(resolve(root, path), "utf8");
const handler = read("plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp");
const discovery = read("plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_Discovery.cpp");
const topology = read("plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintTopologySerializer.cpp");
const request = JSON.parse(read("plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v2.schema.json"));

describe("atomic bridge fixed-struct Pass 1 contract", () => {
  it("advertises only the two fixed struct operations", () => {
    const wire = JSON.stringify(request);
    expect(wire).toContain("graph.add-make-struct");
    expect(wire).toContain("graph.add-break-struct");
    expect(wire).toContain("graph.add-make-struct@1.0");
    expect(wire).toContain("graph.add-break-struct@1.0");
    expect(wire).not.toContain("set-members-in-struct");
  });

  it("discovers exact reflected struct identity and template pin evidence", () => {
    expect(discovery).toContain('TEXT("struct_operation")');
    expect(discovery).toContain('TEXT("type_path")');
    expect(discovery).toContain("UScriptStruct");
    expect(discovery).toContain('TEXT("K2Node_MakeStruct")');
    expect(discovery).toContain('TEXT("K2Node_BreakStruct")');
  });

  it("preflights exact identity before constructing dedicated K2 nodes", () => {
    expect(handler).toContain("ResolveExactStructOperation");
    expect(handler).toContain("/Script/BlueprintGraph.BlueprintFieldNodeSpawner");
    expect(handler).toContain("UK2Node_MakeStruct");
    expect(handler).toContain("UK2Node_BreakStruct");
    expect(handler).toContain("StructNode->StructType = Struct");
    expect(handler.indexOf("ResolveExactStructOperation"))
      .toBeLessThan(handler.indexOf("StructNode->StructType = Struct"));
  });

  it("serializes independent class and struct-operation readback", () => {
    expect(topology).toContain('TEXT("make-struct")');
    expect(topology).toContain('TEXT("break-struct")');
    expect(topology).toContain('TEXT("structOperation")');
    expect(topology).toContain('TEXT("typePath")');
  });
});
