#!/usr/bin/env node
/* Studio UI 确定性拼接构建。
 *
 * 源分片（src/styles/*.css + src/<域>/*.js）按 scripts/build-order.json 记录的
 * 原始行区间重拼为单文件 dist/studio-ui.html。分片是原顶层语句的逐字节切片，
 * 拼接顺序 = 原行序，因此产物与迁移基线逐字节一致——这是 M3"行为零变化"
 * 的机器可验证证据（见 scripts/check-freshness.mjs）。
 *
 * 全局作用域说明：分片共享顶层 var/function（与原文件一致）；ESM 化与
 * store 引入在 M5 执行（迁移地图见 scripts/fragment-deps.json）。
 */
import { readFileSync, writeFileSync, mkdirSync, existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const webRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const src = join(webRoot, "src");
const meta = JSON.parse(readFileSync(join(webRoot, "scripts", "build-order.json"), "utf8"));

// 去掉写入时附加的单个尾换行，还原切片的精确行集
function body(file) {
  const t = readFileSync(join(src, file), "utf8");
  if (!t.endsWith("\n")) throw new Error(`fragment missing trailing newline: ${file}`);
  return t.slice(0, -1);
}

// ---------- CSS ----------
const cssText = meta.css.map((c) => body(c.file)).join("\n");

// ---------- JS ----------
// 1) 每个分片内部：按自身 ranges 顺序游走（ranges 之间的空行已在文件里）
const pieces = [];
for (const frag of meta.js) {
  const lines = body(frag.file).split("\n");
  let cursor = 0;
  let prevEnd = null;
  for (const [s, e] of frag.ranges) {
    if (prevEnd !== null) cursor += s - prevEnd - 1; // 跳过片内空行
    const len = e - s + 1;
    const slice = lines.slice(cursor, cursor + len);
    if (slice.length !== len) throw new Error(`fragment shorter than ranges: ${frag.file}`);
    pieces.push({ s, e, text: slice.join("\n") });
    cursor += len;
    prevEnd = e;
  }
}
// 2) 全局序拼接：语句区间之间的空行按绝对行距补齐
pieces.sort((a, b) => a.s - b.s);
let jsText = "";
let prev = null; // 上一个已拼语句的结束行（1-based）
for (const p of pieces) {
  if (prev !== null && p.s <= prev) throw new Error(`overlapping ranges at line ${p.s}`);
  // 首片直接开始（<script> 后的换行由模板提供）；相邻片 1 个换行，
  // 间隔 n 行空行时共 n+1 个换行，即 repeat(p.s - prev)
  const sep = prev === null ? 0 : p.s - prev;
  jsText += "\n".repeat(sep) + p.text;
  prev = p.e;
}
if (prev !== meta.js_span[1]) throw new Error(`js tiling ends at ${prev}, expect ${meta.js_span[1]}`);

// ---------- HTML ----------
const tpl = readFileSync(join(src, "index.html"), "utf8");
let html = tpl.replace("<!--BUILD:CSS-->", () => `<style>\n${cssText}\n</style>`);
html = html.replace("<!--BUILD:JS-->", () => jsText);
if (html.includes("<!--BUILD:")) throw new Error("unreplaced build marker");

const distDir = join(webRoot, "dist");
mkdirSync(distDir, { recursive: true });
const outPath = join(distDir, "studio-ui.html");
writeFileSync(outPath, html);

// ---------- 与迁移基线的逐字节对比 ----------
const baseline = join(webRoot, "..", "studio_ui.html");
let parity = "baseline-missing";
if (existsSync(baseline)) {
  const a = readFileSync(outPath);
  const b = readFileSync(baseline);
  parity = a.equals(b) ? "byte-identical" : `DIFFERS (${a.length} vs ${b.length} bytes)`;
}
console.log(`build ok: ${outPath} (${html.length} bytes) | baseline parity: ${parity}`);
if (!parity.startsWith("byte")) process.exitCode = 2;
