import { once } from "node:events";
import type { AddressInfo } from "node:net";
import { afterEach, describe, expect, it, vi } from "vitest";
import { BaseTask, TaskRegistry } from "@db-lyon/flowkit";
import type { FlowRunResult } from "@db-lyon/flowkit";

vi.mock("../../src/log.js", () => ({
  info: vi.fn(),
  warn: vi.fn(),
  error: vi.fn(),
}));

import {
  createFlowTool,
  formatFlowResult,
} from "../../src/flow/flow-tool.js";
import { startFlowHttpServer } from "../../src/flow/http-server.js";

const servers: import("node:http").Server[] = [];
const fixtureRecord = {
  objectPath: "/Game/Test/BP_Fixture.BP_Fixture",
  assetClassPath: "/Script/Engine.Blueprint",
};

function runResult(data: Record<string, unknown>): FlowRunResult {
  return {
    success: true,
    duration: 12,
    steps: [{
      stepNumber: 1,
      type: "task",
      name: "fixture",
      skipped: false,
      duration: 10,
      attempts: 1,
      result: { success: true, data },
    }],
  };
}

class FixtureTask extends BaseTask<Record<string, unknown>> {
  get taskName(): string {
    return "fixture";
  }

  async execute() {
    return {
      success: true,
      data: {
        returnValue: {
          success: true,
          inlineRecords: [fixtureRecord],
          receivedRequest: this.options.request,
          receivedInput: this.options.input,
        },
      },
    };
  }
}

async function startFixtureServer() {
  const registry = new TaskRegistry();
  registry.register("fixture", FixtureTask);
  const config = {
    tasks: { fixture: { class_path: "fixture", options: {} } },
    flows: {
      scan: {
        description: "fixture scanner",
        steps: { "1": { task: "fixture" } },
      },
    },
  };
  const flowTool = createFlowTool(registry, () => config);
  const started = startFlowHttpServer(
    flowTool,
    {
      bridge: { isConnected: false },
      project: {},
    } as never,
    { host: "127.0.0.1", port: 0, token: "test-active-token-value" },
  );
  servers.push(started.server);
  await once(started.server, "listening");
  const port = (started.server.address() as AddressInfo).port;
  const request = (path: string, init: RequestInit = {}) =>
    fetch(`http://127.0.0.1:${port}${path}`, {
      ...init,
      headers: {
        Authorization: "Bearer test-active-token-value",
        "Content-Type": "application/json",
        ...init.headers,
      },
    });
  return { request };
}

afterEach(async () => {
  await Promise.all(servers.splice(0).map((server) =>
    new Promise<void>((resolve) => server.close(() => resolve()))
  ));
});

describe("bounded HTTP flow result transport", () => {
  it("keeps normal MCP flow responses compact and preserves summary behavior", () => {
    const compact = formatFlowResult(runResult({
      returnValue: { inlineRecords: [fixtureRecord] },
    }));
    expect(compact.success).toBe(true);
    expect(compact.stepCount).toBe(1);
    expect(compact.summary).toContain("Flow completed");
    expect(compact).not.toHaveProperty("steps");
    expect(JSON.stringify(compact)).not.toContain(fixtureRecord.objectPath);
  });

  it("returns completed task data over HTTP and passes body.params to step options", async () => {
    const { request } = await startFixtureServer();
    const body = {
      params: {
        request: {
          contractVersion: "projectx.state.snapshot@1.1",
          mode: "deep-blueprint-batch",
          assetPaths: [fixtureRecord.objectPath],
        },
      },
    };
    const response = await request("/flows/scan/run", {
      method: "POST",
      body: JSON.stringify(body),
    });
    expect(response.status).toBe(200);
    const result = await response.json() as any;
    expect(result.steps[0].result.data.returnValue.inlineRecords).toEqual([fixtureRecord]);
    expect(result.steps[0].result.data.returnValue.receivedRequest).toEqual(body.params.request);
    expect(result.steps[0].result.data.returnValue.receivedInput).toEqual({
      request: body.params.request,
    });
  });

  it("recursively redacts secret keys and exact active token values", () => {
    const formatted = formatFlowResult(runResult({
      returnValue: {
        Authorization: "Bearer hidden",
        nested: {
          access_token: "hidden",
          api_key: "hidden",
          cookie: "hidden",
          note: "prefix test-active-token-value suffix",
        },
      },
    }), { includeStepData: true, activeToken: "test-active-token-value" });
    const text = JSON.stringify(formatted);
    expect(text).not.toContain("Bearer hidden");
    expect(text).not.toContain("test-active-token-value");
    expect(text).toContain("[REDACTED]");
  });

  it("enforces per-step and total embedded-data limits with valid omission records", () => {
    const perStep = formatFlowResult(runResult({ value: "x".repeat(200) }), {
      includeStepData: true,
      maxStepBytes: 32,
      maxTotalBytes: 1024,
    }) as any;
    expect(perStep.steps[0].result).toMatchObject({
      dataOmitted: true,
      reason: "result_too_large",
      limitBytes: 32,
    });
    expect(perStep.steps[0].result.sha256).toMatch(/^[a-f0-9]{64}$/);

    const twoSteps = runResult({ value: "first" });
    twoSteps.steps.push({
      ...twoSteps.steps[0],
      stepNumber: 2,
      name: "fixture-2",
      result: { success: true, data: { value: "second" } },
    });
    const firstBytes = Buffer.byteLength(JSON.stringify({ value: "first" }));
    const total = formatFlowResult(twoSteps, {
      includeStepData: true,
      maxStepBytes: 1024,
      maxTotalBytes: firstBytes,
    }) as any;
    expect(total.steps[0].result.data).toEqual({ value: "first" });
    expect(total.steps[1].result).toMatchObject({
      dataOmitted: true,
      reason: "total_result_limit",
      limitBytes: firstBytes,
    });
    expect(() => JSON.stringify(total)).not.toThrow();
  });

  it("serializes errors and malformed values without leaking internal context", () => {
    const circular: Record<string, unknown> = {};
    circular.self = circular;
    Object.defineProperty(circular, "broken", {
      enumerable: true,
      get: () => { throw new Error("getter failed"); },
    });
    const result = runResult({
      circular,
      bigint: 12n,
      fn: () => "not serialized",
      bridge: { socket: "internal" },
      project: { projectDir: "internal" },
    });
    result.success = false;
    result.steps[0].result = {
      success: false,
      data: result.steps[0].result!.data,
      error: new Error("failed test-active-token-value"),
    };
    const formatted = formatFlowResult(result, {
      includeStepData: true,
      activeToken: "test-active-token-value",
    }) as any;
    expect(formatted.steps[0].result.error.message).toBe("failed [REDACTED]");
    expect(formatted.steps[0].result.data.circular.self).toBe("[CIRCULAR]");
    expect(formatted.steps[0].result.data.circular.broken).toBe("[UNSERIALIZABLE]");
    expect(formatted.steps[0].result.data.bigint).toBe("12");
    expect(formatted.steps[0].result.data.fn).toBe("[UNSERIALIZABLE]");
    expect(formatted.steps[0].result.data.bridge).toBe("[OMITTED_INTERNAL]");
    expect(formatted.steps[0].result.data.project).toBe("[OMITTED_INTERNAL]");
    expect(() => JSON.stringify(formatted)).not.toThrow();
  });

  it("preserves authentication, flow listing, and health routes", async () => {
    const { request } = await startFixtureServer();
    const health = await request("/health");
    expect(health.status).toBe(200);
    expect(await health.json()).toMatchObject({ ok: true, host: "127.0.0.1" });

    const flows = await request("/flows");
    expect(flows.status).toBe(200);
    expect(await flows.json()).toMatchObject({
      flowCount: 1,
      flows: [{ name: "scan", stepCount: 1 }],
    });

    const denied = await fetch(health.url);
    expect(denied.status).toBe(401);
    expect(await denied.json()).toEqual({ error: "Missing or invalid token" });
  });
});
