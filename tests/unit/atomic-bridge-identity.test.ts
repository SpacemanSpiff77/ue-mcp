import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const contractRoot = resolve(root, "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint");
const annotationKeys = new Set(["$comment", "$id", "$schema", "description", "examples", "title", "x-spacehead-schema-version"]);
const unorderedArrays = new Set(["allOf", "anyOf", "enum", "oneOf", "required", "type"]);

function canonical(value: any, parentKey = ""): string {
  if (value === null || typeof value !== "object") return JSON.stringify(value);
  if (Array.isArray(value)) {
    const items = value.map(item => canonical(item));
    if (unorderedArrays.has(parentKey)) items.sort();
    return `[${items.join(",")}]`;
  }
  return `{${Object.keys(value).filter(key => !annotationKeys.has(key)).sort()
    .map(key => `${JSON.stringify(key)}:${canonical(value[key], key)}`).join(",")}}`;
}

function schemaDigest(file: string): string {
  const parsed = JSON.parse(readFileSync(resolve(contractRoot, file), "utf8"));
  return createHash("sha256").update(canonical(parsed), "utf8").digest("hex");
}

describe("atomic bridge exact build and schema identity", () => {
  it("derives stable structural schema digests from the packaged language-neutral contracts", () => {
    expect(schemaDigest("request-v1.schema.json")).toBe("75de2ee235caf5cc91ea750b665dc18506534a93cacd246995e7fd9fdccea232");
    expect(schemaDigest("receipt-v1.schema.json")).toBe("e6b7233ce96e62b5a739060fa7af3c8a07826d579ec0f3da42886e6984751580");
  });

  it("keeps adjacent Spacehead contract copies canonically identical when both repositories are checked out", () => {
    const spaceheadContracts = resolve(root, "../SpaceheadMCP/contracts/atomic-blueprint");
    if (!existsSync(spaceheadContracts)) return;
    for (const file of ["request-v1.schema.json", "receipt-v1.schema.json"]) {
      const bridge = JSON.parse(readFileSync(resolve(contractRoot, file), "utf8"));
      const spacehead = JSON.parse(readFileSync(resolve(spaceheadContracts, file), "utf8"));
      expect(canonical(bridge)).toBe(canonical(spacehead));
    }
  });

  it("generates commit metadata and fingerprints every qualified atomic implementation input", () => {
    const build = readFileSync(resolve(root, "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/UE_MCP_Bridge.Build.cs"), "utf8");
    const script = readFileSync(resolve(root, "scripts/build.js"), "utf8");
    const handler = readFileSync(resolve(root, "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
    for (const input of [
      "BlueprintHandlers.cpp", "BlueprintHandlers.h", "BlueprintHandlers_BuildSpec.cpp",
      "BlueprintTopologySerializer.cpp", "BlueprintTopologySerializer.h",
      "request-v1.schema.json", "receipt-v1.schema.json",
    ]) expect(build).toContain(input);
    expect(script).toContain("git rev-parse HEAD");
    expect(script).toContain("SPACEHEAD_BRIDGE_GIT_COMMIT");
    expect(script).toContain("generateAtomicBridgeBuildIdentity");
    expect(script).toContain("-NoUBTMakefiles");
    expect(handler).toContain("AtomicBridgeBuildIdentity.generated.h");
    expect(handler).not.toContain("spacehead-pass2-atomic-build@1");
  });
});
