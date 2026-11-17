/* /api/state 契约测试（M4 首切片）：
 * 服务端是权威（hf_studio.py _api_state），前端在 /api 边界用 zod 消费。
 * fixtures/tests/fixtures-state.json 是 Phase 0 从真实服务录制的黄金响应；
 * schema 与 src/schemas/state.schema.json（JSON Schema 提取自服务端）对齐。
 * 服务端加字段 → fixture 更新 + 本测试同步；删字段 → 两端一起红。
 */
import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { z } from "zod";

const webRoot = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

/* 与 src/schemas/state.schema.json 同源的 zod 镜像（运行时校验用）。
 * 修改任何一端必须同 PR 过两端（JSON Schema 由 fixture 对拍兜底）。 */
const sceneSchema = z.object({
  id: z.union([z.string(), z.number()]),
  start: z.number().min(0).optional(),
  dur: z.number().min(0).optional(),
  type: z.string().optional(),
  text: z.string().optional(),
  has_video: z.boolean().optional(),
}).passthrough();

const audioSchema = z.object({
  id: z.union([z.string(), z.number()]),
  src: z.string().optional(),
  start: z.number().optional(),
  dur: z.number().optional(),
  kind: z.string().optional(),
}).passthrough();

const findingSchema = z.object({
  level: z.enum(["error", "warning", "info"]),
  message: z.string(),
});

const stateSchema = z.object({
  spec: z.any().nullable(),
  spec_path: z.string().nullable(),
  duration: z.number().min(0),
  scenes: z.array(sceneSchema),
  audios: z.array(audioSchema),
  width: z.number().int().min(1),
  height: z.number().int().min(1),
  errors: z.number().int().min(0),
  agent: z.object({
    llm: z.boolean(),
    mcp: z.literal(true),
    cli: z.string().nullable(), // 探测到的 CLI 名（claude/codex），未装为 null
  }).strict(),
  runs: z.array(z.object({
    idx: z.number().int().min(0),
    ts: z.string(),
    message: z.string(),
    backend: z.string(),
  }).strict()),
  import: z.object({
    last_dir: z.string().nullable(),
    spec_dir: z.string(),
    upload_max_mb: z.number().int().min(1),
    can_undo: z.boolean(),
    undo_label: z.string().nullable(),
  }).strict(),
  findings: z.array(findingSchema),
});

function fixtureState() {
  return JSON.parse(readFileSync(join(webRoot, "tests", "fixtures-state.json"), "utf8"));
}

describe("/api/state 契约（黄金 fixture）", () => {
  it("录制响应通过 zod schema", () => {
    const r = stateSchema.safeParse(fixtureState());
    if (!r.success) console.error(r.error.issues.slice(0, 5));
    expect(r.success).toBe(true);
  });
  it("关键字段勾稽（fixtures 与服务端实现一致）", () => {
    const d = fixtureState();
    expect(d.errors).toBe(0);
    expect(d.findings.every((f: any) => ["error", "warning", "info"].includes(f.level))).toBe(true);
    expect(d.agent.mcp).toBe(true);
    expect(d.duration).toBeCloseTo(10.5, 5);
    expect(d.scenes.length).toBeGreaterThan(0);
  });
  it("schema 拒绝坏结构（level 越界 / duration 缺失）", () => {
    const bad1 = { ...fixtureState(), findings: [{ level: "fatal", message: "x" }] };
    expect(stateSchema.safeParse(bad1).success).toBe(false);
    const bad2: any = { ...fixtureState() };
    delete bad2.duration;
    expect(stateSchema.safeParse(bad2).success).toBe(false);
  });
  it("spec 字段符合 spec.schema.json 的 error 级约束", () => {
    const d = fixtureState();
    // spec.schema.json 的 error 级：schema_version const 1.0、scenes 非空
    expect((d.spec as any).schema_version).toBe("1.0");
    expect(Array.isArray(d.spec.scenes) && d.spec.scenes.length > 0).toBe(true);
  });
});

/* JSON Schema ↔ zod 对拍：同一 fixture 必须被两端同时接受。
 * JSON Schema 由 python 侧 jsonschema 校验（tools/web/tests/test_schemas.py）。 */
describe("双端契约对拍", () => {
  it("zod 接受 = JSON Schema 接受（同一 fixture）", () => {
    const d = fixtureState();
    expect(stateSchema.safeParse(d).success).toBe(true);
    // JSON Schema 侧的 error 级交集（required 字段 + 类型族）在 TS 侧镜像断言
    const required = ["spec", "spec_path", "duration", "scenes", "audios",
      "width", "height", "errors", "agent", "runs", "import", "findings"];
    const missing = required.filter((k) => !(k in d));
    expect(missing).toEqual([]);
  });
});
