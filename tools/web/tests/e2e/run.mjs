#!/usr/bin/env node
/* Studio UI 端到端基线回归（Playwright）。
 *
 * 用法：先起服务，再跑本脚本——
 *   python3 tools/hf_studio.py <contract.html> --serve --spec <spec.json> --port 8121
 *   cd tools/web && npm run e2e            # BASE_URL 默认 http://127.0.0.1:8121
 *
 * 断言：控制台零 error 级消息 + 关键交互可用（播放推进 / seek / 面板开合）。
 * 截图存 tests/baseline/e2e-*.png，供与 Phase 0 人工基线对照；像素级像素
 * 断言不做——构建层已由 scripts/build.mjs 的逐字节 parity 兜底。
 */
import { chromium } from "playwright";
import { mkdirSync } from "node:fs";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";

const BASE_URL = process.env.BASE_URL || "http://127.0.0.1:8121";
const outDir = join(dirname(fileURLToPath(import.meta.url)), "..", "baseline");
mkdirSync(outDir, { recursive: true });

const errors = [];
let step = 0;
const ok = (msg) => console.log(`  ok[${++step}] ${msg}`);
const fail = (msg) => { console.error(`  FAIL[${step}] ${msg}`); process.exitCode = 1; };

const browser = await chromium.launch();
try {
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  page.on("pageerror", (e) => errors.push(String(e)));

  // 1. 加载与契约校验
  await page.goto(BASE_URL, { waitUntil: "networkidle" });
  if ((await page.title()) !== "xmedia Studio") fail(`title=${await page.title()}`);
  else ok("页面加载，标题正确");
  const status = await page.textContent("#status");
  if (!/校验通过/.test(status || "")) fail(`契约校验: ${status}`);
  else ok(`契约校验通过（${status}）`);
  await page.screenshot({ path: join(outDir, "e2e-01-initial.png") });

  // 2. 播放推进 + 时间码
  const tc0 = await page.textContent("#tc");
  await page.click("#btn-play");
  await page.waitForTimeout(1200);
  await page.click("#btn-play");
  const tc1 = await page.textContent("#tc");
  if (tc0 === tc1) fail(`播放未推进: ${tc1}`);
  else ok(`播放推进 ${tc0.split(" ")[0]} → ${tc1.split(" ")[0]}`);
  await page.screenshot({ path: join(outDir, "e2e-02-after-play.png") });

  // 3. 步进与 seek（步进回传是 postMessage 异步链，需等一拍再读时间码）
  await page.keyboard.press("ArrowRight");
  await page.waitForTimeout(300);
  const tc2 = await page.textContent("#tc");
  if (tc2 === tc1) fail(`ArrowRight 未步进: ${tc2}`);
  else ok(`方向键步进 → ${tc2.split(" ")[0]}`);
  await page.fill("#scrub", "0");
  await page.dispatchEvent("#scrub", "input");
  ok("scrub seek 无异常");

  // 4. 三个面板开合
  await page.click("#ask-fab");
  if (!(await page.locator("#ask-panel.open").count())) fail("Ask AI 面板未打开");
  else ok("Ask AI 面板打开");
  await page.screenshot({ path: join(outDir, "e2e-03-ask.png") });
  await page.click("#ask-fab");

  await page.click("#btn-bake");
  if (!(await page.locator("#exp-panel.open").count())) fail("导出面板未打开");
  else ok("导出面板打开");
  await page.screenshot({ path: join(outDir, "e2e-04-export.png") });
  await page.click("#exp-x");

  await page.click("#btn-import");
  if (!(await page.locator("#imp-panel.open").count())) fail("导入面板未打开");
  else ok("导入面板打开");
  await page.screenshot({ path: join(outDir, "e2e-05-import.png") });
  await page.mouse.click(10, 10); // dialog ::backdrop 点击关闭

  // 5. Storyboard 逐镜审阅（first-video "approve the storyboard" 本地版）
  await page.click("#btn-storyboard");
  await page.waitForTimeout(300);
  const sbCards = await page.locator("#sb-list .sb-card").count();
  const apiSceneCount = await page.evaluate(async () => {
    const st = await (await fetch("/api/state")).json();
    return (st.scenes || []).length;
  });
  if (sbCards !== apiSceneCount) fail(`storyboard 卡数 ${sbCards} ≠ 场景数 ${apiSceneCount}`);
  else ok(`storyboard 逐镜卡 ${sbCards} 张（与 /api/state 一致）`);
  await page.screenshot({ path: join(outDir, "e2e-06-storyboard.png") });
  await page.locator("#sb-list .sb-card .sb-edit").first().click();
  await page.waitForTimeout(300);
  const sbHidden = await page.locator("#sb-panel")
    .evaluate((el) => el.style.display === "none");
  if (!sbHidden) fail("「改这镜」后 storyboard 面板未收起");
  else if (!(await page.locator(".sc-card.sel").count()))
    fail("「改这镜」后场景卡未选中");
  else ok("「改这镜」定位链（面板收起 + 场景选中）");

  // 6. /api/state 可用
  const state = await page.evaluate(async () => {
    const r = await fetch("/api/state");
    return r.json();
  });
  if (!state.spec || !("scenes" in state)) fail("/api/state 结构异常");
  else ok(`/api/state: ${state.scenes.length} 场景 / ${state.duration}s`);

  if (errors.length) {
    fail(`控制台 ${errors.length} 条 error: ${errors[0]}`);
  } else {
    ok("控制台零 error");
  }
} finally {
  await browser.close();
}
console.log(process.exitCode ? "e2e: FAIL" : "e2e: PASS");
