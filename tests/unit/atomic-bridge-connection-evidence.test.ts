import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const handler = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");
const receipt = JSON.parse(readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Contracts/AtomicBlueprint/receipt-v2.schema.json"), "utf8"));

describe("atomic bridge durable direct-connection decision evidence", () => {
  it("serializes every UE 5.8 ECanCreateConnectionResponse without changing acceptance", () => {
    for (const response of [
      "CONNECT_RESPONSE_MAKE",
      "CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE",
      "CONNECT_RESPONSE_MAKE_WITH_PROMOTION",
      "CONNECT_RESPONSE_BREAK_OTHERS_A",
      "CONNECT_RESPONSE_BREAK_OTHERS_B",
      "CONNECT_RESPONSE_BREAK_OTHERS_AB",
      "CONNECT_RESPONSE_DISALLOW",
    ]) {
      expect(handler).toContain(`case ${response}:`);
      expect(receipt.$defs.connectionDecision.properties.unreal_response_enum.enum).toContain(response);
    }
    expect(handler).toContain("CompatibilityResponse.Response == CONNECT_RESPONSE_MAKE");
    expect(handler).toContain("DirectConnectionResponse.Response == CONNECT_RESPONSE_MAKE");
    expect(handler).not.toContain("CreateAutomaticConversionNodeAndConnections");
    expect(handler).not.toContain("CreatePromotedConnection");
  });

  it("binds normalized authority, pins, structural state, mutation, and reciprocal proof", () => {
    expect(receipt.$defs.connectionDecision.properties).toMatchObject({
      evidence_version: { const: "spacehead.connection-decision-evidence@1.0" },
      profile: { const: "direct_exact_v1" },
      unreal_response_code: { type: "integer" },
      fatal: { type: "boolean" },
      diagnostic_text: { type: "string", maxLength: 256 },
      structural_profile_match: { type: "boolean" },
      pins_clean: { type: "boolean" },
      try_create_connection_attempted: { type: "boolean" },
      try_create_connection_succeeded: { type: "boolean" },
      reciprocal_topology_verified: { type: "boolean" },
      outcome: { enum: ["QUALIFIED", "REJECTED"] },
      qualified: { type: "boolean" },
    });
    expect(receipt.$defs.connectionDecision.properties.from.$ref).toBe("#/$defs/connectionPin");
    expect(receipt.$defs.connectionDecision.properties.to.$ref).toBe("#/$defs/connectionPin");
    expect(handler).toContain("Response.IsFatal()");
    expect(handler).toContain("static_cast<int32>(Response.Response.GetValue())");
    expect(handler).toContain("FromPreLinkCount");
    expect(handler).toContain("ToPreLinkCount");
    expect(handler).toContain("bTrySucceeded && FromPin->LinkedTo.Contains(ToPin) && ToPin->LinkedTo.Contains(FromPin)");
  });

  it("persists decisions in terminal Stage B and Stage C receipt evidence", () => {
    expect(receipt.$defs.evidence.properties.connection_decisions).toMatchObject({
      type: "array", maxItems: 8, items: { $ref: "#/$defs/connectionDecision" },
    });
    expect(receipt.$defs.operationResult.properties.connection_decision.$ref)
      .toBe("#/$defs/connectionDecision");
    expect(handler.match(/SetArrayField\(TEXT\("connection_decisions"\)/g)?.length).toBeGreaterThanOrEqual(3);
    expect(handler).toContain('Result->SetObjectField(TEXT("connection_decision"), ConnectionDecision)');
  });

  it("uses stable reason codes and keeps diagnostic prose non-authoritative", () => {
    for (const code of ["CONVERSION_REQUIRED", "PROMOTION_REQUIRED", "BREAK_EXISTING_REQUIRED",
      "SCHEMA_DISALLOW", "STRUCTURAL_PROFILE_MISMATCH", "PINS_NOT_CLEAN",
      "TRY_CREATE_CONNECTION_NOT_ATTEMPTED", "TRY_CREATE_CONNECTION_FAILED",
      "RECIPROCAL_TOPOLOGY_NOT_VERIFIED"])
      expect(handler).toContain(`TEXT("${code}")`);
    expect(handler).toContain('Diagnostic.ReplaceInline(TEXT("\\r"), TEXT(" "))');
    expect(handler).toContain("Response.Message.ToString().Left(256)");
  });
});
