import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handlerPath = resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp");
const handler = readFileSync(handlerPath, "utf8");
const request = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/request-v1.schema.json"), "utf8"));
const receipt = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/receipt-v1.schema.json"), "utf8"));

describe("atomic Builder Pass 3 recovery boundary", () => {
  it("persists integrity-bound correlated status outside Content and reloads it after restart", () => {
    expect(handler).toContain('FPaths::ProjectSavedDir(), TEXT("SpaceheadBuilder"), TEXT("Transactions")');
    expect(handler).toContain("spacehead.blueprint-atomic-bridge.durable-status@1.0");
    expect(handler).toContain("integrity_sha256");
    expect(handler).toContain("ReadDurableExecution(TransactionId, CorrelationId");
    expect(handler).toContain("CORRELATED_STATUS_CORRUPT");
    expect(handler).toContain("EXECUTION_IN_PROGRESS_OR_INTERRUPTED");
    expect(handler).toContain("AtomicReplaceUtf8");
  });

  it("keeps persistent monotonic fencing and rejects corrupt or non-increasing records", () => {
    expect(handler).toContain('TEXT("SpaceheadBuilder"), TEXT("Fencing")');
    expect(handler).toContain('Sha256(EnvironmentDigest + TEXT("|") + TargetIdentity)');
    expect(handler).toContain("Fields.Num() != 3 || !Fields[0].IsNumeric()");
    expect(handler).toContain("Sequence <= ExistingSequence");
    expect(handler).toContain("Handle->Flush(true)");
  });

  it("gates fault checkpoints to the general test project plus the exact Phase E rollback probe", () => {
    const hooks = request.$defs.testHooks;
    expect(hooks.required).toEqual(["test_hook_version", "checkpoint"]);
    expect(hooks.additionalProperties).toBe(false);
    expect(hooks.properties.test_hook_version.const)
      .toBe("spacehead.blueprint-atomic-bridge.test-hooks@1.0");
    expect(hooks.properties.checkpoint.enum).toEqual(expect.arrayContaining([
      "BEFORE_PREFLIGHT", "AFTER_PREFLIGHT_BEFORE_MUTATION", "AFTER_MUTATION", "BEFORE_COMPILE",
      "AFTER_COMPILE_BEFORE_VERIFY", "VERIFICATION_FAILURE", "AFTER_VERIFY_BEFORE_SAVE", "SAVE_FAILURE",
      "AFTER_SAVE_BEFORE_FINAL_RECEIPT", "ROLLBACK_FAILURE",
    ]));
    expect(handler).toContain('ProjectName.Equals(TEXT("ue_mcp")');
    expect(handler).toContain('PackagePath.StartsWith(TEXT("/Game/Tests/Builder/"))');
    expect(handler).toContain("Hooks->Values.Num() != 2");
    expect(handler).toContain('PackagePath == TEXT("/Game/Tests/Builder/BP_PhaseE_WholeFunction")');
    expect(handler).toContain('Checkpoint == TEXT("AFTER_FINAL_MUTATION")');
    expect(handler).not.toContain("RequestExit(");
    expect(handler).not.toContain("TerminateProc(");
  });

  it("records one dispatch and exposes recovery-safe receipt vocabulary", () => {
    expect(receipt.$defs.evidence.properties.dispatch_count.maximum).toBe(1);
    expect(receipt.$defs.failure.properties.phase.enum).toContain("RECOVERY");
    expect(receipt.$defs.failure.properties.phase.enum).toContain("RESTART");
    expect(handler).toContain('SetNumberField(TEXT("dispatch_count"), 1)');
    expect(handler).toContain("TEST_ONLY_FINAL_RECEIPT_SUPPRESSED");
    expect(handler).toContain("DURABLE_STATUS_WRITE_FAILED");
  });
});
