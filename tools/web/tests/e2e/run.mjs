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

const HOME_URL = process.env.HOME_URL || "";

/* Home 阶段（HF Studio app Home：项目管理 → 一句话建片 → 编辑页）。
 * 需要独立起一个 --serve --home 服务（XMEDIA_STUDIO_PROJECTS 指向临时
 * 目录），HOME_URL 指向它；未设置时跳过本阶段。 */
async function homePhase(browser) {
  if (!HOME_URL) {
    console.log("  skip Home 阶段（未设 HOME_URL）");
    return;
  }
  const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
  const errs = [];
  page.on("console", (m) => { if (m.type() === "error") errs.push(m.text()); });
  page.on("pageerror", (e) => errs.push(String(e)));
  try {
    await page.goto(HOME_URL, { waitUntil: "networkidle" });
    const hasForm = await page.locator("#message").count() &&
        (await page.locator("#arc").count()) &&
        (await page.locator("#create").count());
    if (!hasForm) fail("Home 页缺建片表单（message/arc/create）");
    else ok("Home 项目管理页加载（message/arc/create 表单）");
    await page.screenshot({ path: join(outDir, "e2e-07-home.png") });

    await page.fill("#message", "e2e Home 冒烟：一句话建片");
    await page.selectOption("#arc", "problem-solution");
    await page.click("#create");
    await page.waitForURL("**/project/**", { timeout: 15000 });
    ok("创建成功 → " + page.url().split("/").slice(-1)[0]);

    await page.waitForTimeout(800);
    const status = await page.textContent("#status");
    if (!/校验通过/.test(status || "")) fail(`编辑页契约校验: ${status}`);
    else ok("项目编辑页加载，契约校验通过");
    await page.screenshot({ path: join(outDir, "e2e-08-home-editor.png") });

    const projects = await page.evaluate(async () =>
      (await (await fetch("/api/projects")).json()).projects);
    const names = projects.map((p) => (typeof p === "string" ? p : p.name));
    if (!names.length) fail("/api/projects 项目列表为空");
    else ok(`/api/projects：${names.length} 个项目`);
    if (errs.length) fail(`Home 阶段控制台 ${errs.length} 条 error: ${errs[0]}`);
    else ok("Home 阶段控制台零 error");
  } finally {
    await page.close();
  }
}

const browser = await chromium.launch();
await homePhase(browser);

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

  // 4. chat 主界面化：对话面板 = 左栏第一面板，默认展开（HF 三栏形态）
  const chatVisible = await page.locator("#chat-panel")
    .evaluate((el) => el.style.display !== "none");
  if (!chatVisible) fail("chat 对话面板未常驻（应为左栏第一面板默认展开）");
  else ok("chat 对话面板常驻左栏第一面板（HF 三栏形态）");
  const chatInput = await page.locator("#ask-input").count();
  if (!chatInput) fail("chat 对话输入框缺失");
  else ok("chat 对话输入框在位");
  await page.screenshot({ path: join(outDir, "e2e-03-chat.png") });

  // 4b. 模板库 tab：19 个模板卡片可浏览（HF catalog 可发现性对齐）
  await page.click("#tab-templates");
  await page.waitForTimeout(400);
  const tplCards = await page.locator("#templates-panel .tpl-card").count();
  if (tplCards < 19) fail(`模板库卡片 ${tplCards} < 19`);
  else ok(`模板库 tab：${tplCards} 个模板卡片可浏览/可插入`);

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

  // 5. Storyboard 逐镜审阅（first-video "approve the storyboard" 本地版；
  //    M5+：第四个 tab「分镜」+ 点画面选中（不跳走）+ 卡上内联编辑）
  await page.click("#tab-storyboard");
  await page.waitForTimeout(400);
  const sbCards = await page.locator("#sb-list .sb-card").count();
  const apiSceneCount = await page.evaluate(async () => {
    const st = await (await fetch("/api/state")).json();
    return (st.scenes || []).length;
  });
  if (sbCards !== apiSceneCount) fail(`storyboard 卡数 ${sbCards} ≠ 场景数 ${apiSceneCount}`);
  else ok(`storyboard 逐镜卡 ${sbCards} 张（与 /api/state 一致）`);
  await page.screenshot({ path: join(outDir, "e2e-06-storyboard.png") });
  // 点画面 = 选中该镜（面板保持打开，Inspector 联动不跳走）
  await page.locator("#sb-list .sb-card .sb-shot").first().click();
  await page.waitForTimeout(300);
  const shotSel = await page.locator("#sb-list .sb-card.sel").count() &&
      (await page.locator(".sc-card.sel").count());
  if (!shotSel) fail("点画面后分镜卡/场景卡未选中");
  else ok("点画面选中该镜（分镜保持打开 + Inspector 联动）");
  // 「改这镜」= 定位 + 面板收起
  await page.locator("#sb-list .sb-card .sb-edit").first().click();
  await page.waitForTimeout(300);
  const sbHidden = await page.locator("#sb-panel")
    .evaluate((el) => el.style.display === "none");
  if (!sbHidden) fail("「改这镜」后 storyboard 面板未收起");
  else if (!(await page.locator(".sc-card.sel").count()))
    fail("「改这镜」后场景卡未选中");
  else ok("「改这镜」定位链（面板收起 + 场景选中）");
  // 内联编辑：卡上改文案 → 应用 → /api/recompose → spec 同步
  await page.click("#tab-storyboard");
  await page.waitForTimeout(400);
  const editText = `e2e 内联编辑 ${Date.now()}`;
  await page.locator("#sb-list .sb-card .sb-edit-text").first()
    .fill(editText);
  await page.locator("#sb-list .sb-card .sb-apply").first().click();
  await page.waitForTimeout(1500);
  const after = await page.evaluate(async () => {
    const st = await (await fetch("/api/state")).json();
    return (st.spec.scenes[0].text || "");
  });
  if (after !== editText)
    fail(`内联编辑未写回 spec（${JSON.stringify(after)}）`);
  else ok("分镜卡内联编辑 → /api/recompose → spec 同步");

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
