import { TaskRegistry } from "@db-lyon/flowkit";
import type { ToolDef } from "../types.js";
/**
 * Walk all category tools and register every action as a flowkit task.
 *
 * - Bridge actions → factory classes with method + mapParams in closure
 * - Handler actions → factory classes wrapping the existing handler function
 *
 * Also registers `ue-mcp.bridge` as a class_path for YAML-defined bridge tasks.
 */
export declare function buildFlowRegistry(tools: ToolDef[]): TaskRegistry;
