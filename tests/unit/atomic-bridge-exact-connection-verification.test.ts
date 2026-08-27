import { readFileSync } from "node:fs";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

const root = resolve(import.meta.dirname, "../..");
const source = readFileSync(resolve(root,
  "plugin/ue_mcp_bridge/Source/UE_MCP_Bridge/Private/Handlers/BlueprintHandlers_BuildSpec.cpp"), "utf8");

type Response = "MAKE" | "BREAK_A" | "BREAK_B" | "BREAK_AB";
type Edge = Readonly<{ graph: string; from: string; fromPin: string; to: string; toPin: string;
  classification: "execution" | "data" }>;

const key = (edge: Edge) => `${edge.graph}|${edge.classification}:${edge.from}:${edge.fromPin}->${edge.to}:${edge.toPin}`;

function apply(expected: Map<string, Edge>, edge: Edge, response: Response): void {
  if (response === "BREAK_A" || response === "BREAK_AB")
    for (const [identity, current] of expected)
      if (current.from === edge.from && current.fromPin === edge.fromPin
          || current.to === edge.from && current.toPin === edge.fromPin) expected.delete(identity);
  if (response === "BREAK_B" || response === "BREAK_AB")
    for (const [identity, current] of expected)
      if (current.from === edge.to && current.fromPin === edge.toPin
          || current.to === edge.to && current.toPin === edge.toPin) expected.delete(identity);
  expected.set(key(edge), edge);
}

function exact(expected: readonly Edge[], observed: readonly Edge[]): boolean {
  const expectedKeys = new Set(expected.map(key));
  const observedKeys = observed.map(key);
  return observedKeys.length === new Set(observedKeys).size
    && expectedKeys.size === observedKeys.length
    && observedKeys.every(identity => expectedKeys.has(identity));
}

const entryReturn: Edge = { graph: "function:A1R", from: "terminal:entry:create_fn", fromPin: "then",
  to: "terminal:return:create_fn", toPin: "execute", classification: "execution" };
const entryBranch: Edge = { ...entryReturn, to: "logical:branch", toPin: "execute" };
const valueBranch: Edge = { graph: "function:A1R", from: "logical:random_bool", fromPin: "ReturnValue",
  to: "logical:branch", toPin: "Condition", classification: "data" };
const valueReturn: Edge = { ...valueBranch, to: "terminal:return:create_fn", toPin: "Result" };

describe("atomic created-function exact connection verification", () => {
  it("tracks exact canonical identities instead of additive shell cardinality", () => {
    expect(source).toContain("ExpectedWholeConnections");
    expect(source).toContain("ObservedWholeConnections");
    expect(source).toContain("WholeConnectionKey");
    expect(source).not.toContain("ShellConnectionCount + WholeConnections.Num()");
  });

  it("updates the expected identity set for every admitted native break response", () => {
    expect(source).toContain("Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A");
    expect(source).toContain("Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B");
    expect(source).toContain("Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB");
    expect(source).toContain("RemoveExpectedConnectionsForPin");
  });

  it("replaces the automatic Entry-to-Return edge and preserves the three A1R edges exactly", () => {
    const expected = new Map([[key(entryReturn), entryReturn]]);
    apply(expected, entryBranch, "BREAK_A");
    apply(expected, valueBranch, "MAKE");
    apply(expected, valueReturn, "MAKE");
    expect([...expected.keys()].sort()).toEqual([key(entryBranch), key(valueBranch), key(valueReturn)].sort());
    expect(expected.has(key(entryReturn))).toBe(false);
  });

  it("rejects missing and unexpected connections", () => {
    const expected = [entryBranch, valueBranch, valueReturn];
    expect(exact(expected, expected.slice(0, 2))).toBe(false);
    expect(exact(expected, [...expected, entryReturn])).toBe(false);
  });

  it("rejects duplicate observations", () => {
    const expected = [entryBranch, valueBranch, valueReturn];
    expect(exact(expected, [...expected, entryBranch])).toBe(false);
  });

  it("rejects wrong pin, source node, destination node, and graph identities", () => {
    for (const wrong of [
      { ...entryBranch, fromPin: "Wrong" },
      { ...entryBranch, from: "logical:random_bool" },
      { ...entryBranch, to: "terminal:return:create_fn" },
      { ...entryBranch, graph: "function:Other" },
    ]) expect(exact([entryBranch], [wrong])).toBe(false);
  });

  it("keeps ordinary CallFunction data fanout exact", () => {
    const expected = new Map<string, Edge>();
    apply(expected, valueBranch, "MAKE");
    apply(expected, valueReturn, "MAKE");
    expect(exact([valueBranch, valueReturn], [...expected.values()])).toBe(true);
  });

  it("supports existing and planned identities in every endpoint combination", () => {
    for (const [from, to] of [
      ["existing:a", "existing:b"], ["logical:a", "existing:b"],
      ["existing:a", "logical:b"], ["logical:a", "logical:b"],
    ]) {
      const edge: Edge = { graph: "graph:G", from, fromPin: "Out", to, toPin: "In", classification: "data" };
      expect(exact([edge], [edge])).toBe(true);
    }
  });

  it("emits exact mismatch, duplicate, automatic-wire, and canonicalization evidence", () => {
    for (const field of ["expected_connection_count", "observed_connection_count", "expected_connections",
      "observed_connections", "missing_expected_connections", "unexpected_observed_connections",
      "semantically_identical_but_compare_unequal", "duplicate_observations",
      "automatically_generated_connections_still_present", "canonical_connection_identity",
      "canonicalization", "source_node_guid", "destination_node_guid", "function_identity"])
      expect(source).toContain(`TEXT("${field}")`);
  });

  it("keeps compile-before-verify, save-last, and exact rollback fencing intact", () => {
    const compile = source.indexOf("CompileWithoutSave(Blueprint, CompileEvidence)");
    const verify = source.indexOf("ObservedWholeConnections", compile);
    const save = source.indexOf("UEditorAssetLibrary::SaveLoadedAsset(Blueprint, false)", verify);
    expect(compile).toBeGreaterThanOrEqual(0);
    expect(verify).toBeGreaterThan(compile);
    expect(save).toBeGreaterThan(verify);
    expect(source).toContain("FBlueprintEditorUtils::RemoveGraph(Blueprint, NewGraph, EGraphRemoveFlags::MarkTransient)");
    expect(source).toContain("semantic_restoration_verified");
  });
});
