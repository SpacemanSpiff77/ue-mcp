import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const request = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v2.schema.json"), "utf8"));

describe("atomic bridge environment-bound fencing", () => {
  it("requires the complete current environment and qualification identity in every fence", () => {
    expect(request.$defs.fencing.required).toEqual(expect.arrayContaining([
      "environment_digest", "bridge_git_commit", "bridge_build_fingerprint",
      "plugin_build_identity", "capability_qualification_digest",
    ]));
    expect(request.$defs.fencing.additionalProperties).toBe(false);
  });

  it("accepts fencing only after recomputing the authoritative current environment", () => {
    expect(handler).toContain("CurrentEnvironmentDigest(CurrentDigest)");
    expect(handler).toContain("EnvironmentDigest == CurrentDigest");
    expect(handler).toContain("AcceptFencing(TargetIdentity, EnvironmentDigest, TransactionId, Token");
  });

  it("rejects a bridge commit change after fence capture", () => {
    expect(handler).toContain("BridgeCommit == UE_MCP_AtomicBuildIdentity::GitCommit");
  });

  it("rejects a bridge fingerprint or plugin identity change after fence capture", () => {
    expect(handler).toContain("BuildFingerprint == UE_MCP_AtomicBuildIdentity::BuildFingerprint");
    expect(handler).toContain("PluginBuildIdentity == UE_MCP_AtomicBuildIdentity::PluginBuildIdentity");
  });

  it("binds the fence to the exact plan qualification set", () => {
    expect(handler).toContain('Plan->TryGetStringField(TEXT("capability_qualification_digest"), PlanQualificationDigest)');
    expect(handler).toContain("QualificationDigest == PlanQualificationDigest");
  });

  it("uses an environment-specific durable sequence namespace after rebuild", () => {
    expect(handler).toContain('Sha256(EnvironmentDigest + TEXT("|") + TargetIdentity)');
    expect(handler).toContain("FencingFile(TargetIdentity, EnvironmentDigest)");
  });

  it("preserves fail-closed monotonic and corrupt-record behavior", () => {
    expect(handler).toContain("Fields.Num() != 3 || !Fields[0].IsNumeric()");
    expect(handler).toContain("Sequence <= ExistingSequence");
    expect(handler).toContain("Handle->Flush(true)");
    expect(handler).toContain('TEXT("STALE_OR_INVALID_FENCING")');
  });
});
