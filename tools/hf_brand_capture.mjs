#!/usr/bin/env node
/* 真实无头浏览器品牌捕获器（Playwright Chromium 优先，回退系统 Chrome）：
 * 1) 先识别防爬验证/挑战页（命中则安全退出并标记 blocked，绝不污染品牌数据）
 * 2) 捕获真实 computedStyle 色板/字体/字体角色(display/body)/按钮组件/圆角/Logo(含 <use> 内联)
 * 3) 捕获首屏截图 + 中后段产品界面分区截图（product_surfaces）与核心文案摘要
 */
import { createRequire } from "node:module";
import { existsSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const require = createRequire(import.meta.url);
const __dir = dirname(fileURLToPath(import.meta.url));

function parseArgs(argv) {
  const out = { url: "", shot: "", surfacesDir: "", name: "site", timeout: 24000 };
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--url") out.url = argv[++i] || "";
    else if (argv[i] === "--shot") out.shot = argv[++i] || "";
    else if (argv[i] === "--surfaces-dir") out.surfacesDir = argv[++i] || "";
    else if (argv[i] === "--name") out.name = argv[++i] || "site";
    else if (argv[i] === "--timeout") out.timeout = Number(argv[++i]) || 24000;
  }
  return out;
}

function loadPlaywright() {
  const localPw = join(__dir, "web", "node_modules", "playwright");
  if (existsSync(localPw)) return require(localPw);
  return require("playwright");
}

function isChallengePage(title, bodyText) {
  const t = String(title || "");
  const b = String(bodyText || "");
  if (/(验证码|安全验证|人机验证|访问验证|Just a moment|Attention Required|Access Denied|Are you a robot|Robot or human|Security Check|Verify you are human)/i.test(t)) {
    return true;
  }
  if (b.length < 480 && /(请完成验证|滑块验证|human verification|cf-browser-verification|Checking your browser|geetest|turnstile)/i.test(b)) {
    return true;
  }
  return false;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  if (!args.url) throw new Error("missing --url");
  const { chromium } = loadPlaywright();

  const launchOpts = { headless: true, args: ["--no-sandbox", "--disable-gpu"] };
  const sysChrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
  if (!existsSync(chromium.executablePath()) && existsSync(sysChrome)) {
    launchOpts.executablePath = sysChrome;
  }

  const browser = await chromium.launch(launchOpts);
  try {
    const context = await browser.newContext({
      viewport: { width: 1280, height: 720 },
      deviceScaleFactor: 1,
      userAgent:
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 " +
        "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36",
    });
    const page = await context.newPage();
    const sheetTexts = [];
    let sheetBytes = 0;
    page.on("response", async (res) => {
      try {
        if (sheetBytes > 1_800_000) return;
        const rt = res.request().resourceType();
        const ct = (res.headers()["content-type"] || "").toLowerCase();
        if (rt === "stylesheet" || ct.includes("text/css")) {
          const txt = await res.text().catch(() => "");
          if (txt && txt.length < 1_000_000) {
            sheetTexts.push(txt);
            sheetBytes += txt.length;
          }
        }
      } catch {}
    });
    await page.goto(args.url, { waitUntil: "domcontentloaded", timeout: args.timeout });
    await page.waitForLoadState("networkidle", { timeout: 4500 }).catch(() => {});
    await page.waitForTimeout(500);

    const probe = await page.evaluate(() => ({
      title: document.title || "",
      text: (document.body ? document.body.innerText : "").trim().slice(0, 800),
      scrollH: document.documentElement ? document.documentElement.scrollHeight : 720,
    }));

    if (isChallengePage(probe.title, probe.text)) {
      console.log(JSON.stringify({
        ok: false,
        blocked: true,
        reason: "anti_bot_challenge",
        site_title: probe.title,
        url: args.url,
      }));
      return;
    }

    const extraShots = [];
    if (args.shot) {
      mkdirSync(dirname(args.shot), { recursive: true });
      await page.screenshot({ path: args.shot, type: "jpeg", quality: 86 });
    }
    if (args.surfacesDir && probe.scrollH > 1100) {
      mkdirSync(args.surfacesDir, { recursive: true });
      const cuts = [
        { pct: 0.42, suffix: "-mid.jpg", label: "核心内容区截屏" },
        { pct: 0.80, suffix: "-low.jpg", label: "下方区块与页尾截屏" },
      ];
      for (const c of cuts) {
        const y = Math.max(400, Math.round(probe.scrollH * c.pct));
        await page.evaluate((top) => window.scrollTo(0, top), y);
        await page.waitForTimeout(350);
        const fn = `${args.name}${c.suffix}`;
        await page.screenshot({ path: join(args.surfacesDir, fn), type: "jpeg", quality: 84 });
        extraShots.push({ filename: fn, label: c.label });
      }
      await page.evaluate(() => window.scrollTo(0, 0));
    }

    const data = await page.evaluate((rawSheets) => {
      function toHex(rgb) {
        if (!rgb || rgb === "transparent") return null;
        const m = rgb.match(/rgba?\(\s*(\d+)[,\s]+(\d+)[,\s]+(\d+)(?:[,\s/]+([0-9.]+))?\s*\)/i);
        if (!m) return null;
        if (m[4] !== undefined && parseFloat(m[4]) < 0.15) return null;
        const h = (n) => Number(n).toString(16).padStart(2, "0");
        return "#" + h(m[1]) + h(m[2]) + h(m[3]);
      }
      function lum(hx) {
        const r = parseInt(hx.slice(1, 3), 16);
        const g = parseInt(hx.slice(3, 5), 16);
        const b = parseInt(hx.slice(5, 7), 16);
        return (r * 299 + g * 587 + b * 114) / 1000;
      }
      function sat(hx) {
        const r = parseInt(hx.slice(1, 3), 16);
        const g = parseInt(hx.slice(3, 5), 16);
        const b = parseInt(hx.slice(5, 7), 16);
        return Math.max(r, g, b) - Math.min(r, g, b);
      }

      const bodyCs = getComputedStyle(document.body);
      const htmlCs = getComputedStyle(document.documentElement);
      const bg = toHex(bodyCs.backgroundColor) || toHex(htmlCs.backgroundColor) || "#ffffff";
      const textColor = toHex(bodyCs.color) || (lum(bg) > 128 ? "#111318" : "#f0f0f5");

      const colorMap = new Map();
      const bgMap = new Map();
      const fontMap = new Map();
      const styleMap = new Map();
      const radMap = new Map();
      const gapMap = new Map();
      const comps = [];
      const seenComp = new Set();

      const addFreq = (map, k) => {
        if (!k) return;
        map.set(k, (map.get(k) || 0) + 1);
      };

      const els = Array.from(
        document.querySelectorAll(
          "header, nav, main, section, h1, h2, h3, p, a, button, [role='button'], " +
          "[class*='btn'], [class*='card'], [class*='tag'], [class*='badge'], " +
          "[class*='logo'], svg, svg path, svg circle, svg rect"
        )
      ).slice(0, 420);

      for (const el of els) {
        const rect = el.getBoundingClientRect();
        if (rect.width <= 0 || rect.height <= 0) continue;
        const cs = getComputedStyle(el);
        const fg = toHex(cs.color);
        const bgc = toHex(cs.backgroundColor);
        const bdc = toHex(cs.borderColor);
        const fillc = toHex(cs.fill);
        if (fg) addFreq(colorMap, fg);
        if (bgc) {
          addFreq(colorMap, bgc);
          addFreq(bgMap, bgc);
        }
        if (fillc && fillc !== "#000000") addFreq(colorMap, fillc);
        if (bdc && parseFloat(cs.borderWidth) > 0) addFreq(colorMap, bdc);

        const fam = (cs.fontFamily || "").split(",")[0]?.replace(/["']/g, "").trim();
        if (fam && !/^(system-ui|sans-serif|serif|monospace|-apple-system|BlinkMacSystemFont)$/i.test(fam)) {
          addFreq(fontMap, fam);
        }
        const fsz = Math.round(parseFloat(cs.fontSize) || 0);
        const fw = parseInt(cs.fontWeight, 10) || 400;
        const lhRaw = parseFloat(cs.lineHeight);
        const lh = fsz > 0 && Number.isFinite(lhRaw) ? +(lhRaw / fsz).toFixed(2) : 1.4;
        if (fsz >= 11 && fsz <= 160) {
          const sk = fsz + ":" + fw + ":" + lh + ":" + (fam || "");
          styleMap.set(sk, (styleMap.get(sk) || 0) + 1);
        }
        const rad = cs.borderRadius;
        if (rad && rad !== "0px" && /^\d+(\.\d+)?px$/.test(rad.split(" ")[0])) {
          addFreq(radMap, rad.split(" ")[0]);
        }
        if (cs.gap && cs.gap !== "normal" && /^\d+(\.\d+)?px$/.test(cs.gap.split(" ")[0])) {
          addFreq(gapMap, cs.gap.split(" ")[0]);
        }
      }

      // 独立组件扫描：按钮 / 输入框 / 标签 / 卡片（允许 border / shadow / gradient，不止依赖实色 bg）
      const compSelectors = [
        ["button", "button, [role='button'], a[class*='btn' i], div[class*='btn' i], span[class*='btn' i], [class*='upload' i], [class*='login' i], [class*='cta' i]"],
        ["input", "input[type='search'], input[type='text'], form[class*='search' i], [class*='search-input' i]"],
        ["tag", "[class*='tag' i], [class*='badge' i], [class*='pill' i], [class*='channel-link' i]"],
        ["card", "[class*='video-card' i], [class*='card' i], [class*='panel' i]"],
      ];
      for (const [kind, q] of compSelectors) {
        const nodes = Array.from(document.querySelectorAll(q)).slice(0, 28);
        for (const el of nodes) {
          if (comps.length >= 10) break;
          const rect = el.getBoundingClientRect();
          if (rect.width < 24 || rect.height < 16 || rect.width > 960) continue;
          const cs = getComputedStyle(el);
          const bgc = toHex(cs.backgroundColor) || "";
          const bdW = parseFloat(cs.borderTopWidth) || 0;
          const bdC = bdW > 0 ? (toHex(cs.borderTopColor) || "") : "";
          const bdStr = (bdW > 0 && bdC) ? `${Math.round(bdW)}px ${cs.borderTopStyle || "solid"} ${bdC}` : "";
          const shd = (cs.boxShadow && cs.boxShadow !== "none") ? cs.boxShadow.slice(0, 80) : "";
          const rad = (cs.borderRadius || "").split(" ")[0] || "";
          const hasBgImg = cs.backgroundImage && cs.backgroundImage !== "none" && cs.backgroundImage.includes("gradient");
          if (!bgc && !bdStr && !shd && !hasBgImg) continue;
          if (kind === "card" && !rad && !bdStr && !shd && !bgc) continue;
          const fg = toHex(cs.color) || textColor;
          const cls = typeof el.className === "string" ? el.className.trim().split(/\s+/)[0] : "";
          const sel = (el.tagName.toLowerCase() + (cls ? "." + cls : "")).slice(0, 48);
          const lbl = (el.innerText || el.getAttribute("placeholder") || el.getAttribute("aria-label") || "").replace(/\s+/g, " ").trim().slice(0, 18);
          const key = kind + "|" + bgc + "|" + rad + "|" + bdStr;
          if (seenComp.has(key)) continue;
          seenComp.add(key);
          comps.push({
            kind,
            selector: sel,
            label: lbl,
            bg: bgc,
            bg_image: hasBgImg ? cs.backgroundImage.slice(0, 120) : "",
            color: fg,
            border: bdStr,
            shadow: shd,
            radius: rad && rad !== "0px" ? rad : "",
            padding: cs.padding || "",
            font_size: Math.round(parseFloat(cs.fontSize) || 14),
            font_weight: parseInt(cs.fontWeight, 10) || 400,
          });
        }
      }

      // 从已加载 CSSOM 读取同源/允许样式表中的 CSS 变量与字体族
      let cssVarCount = 0;
      const cssVarsExtracted = [];
      for (const sheet of Array.from(document.styleSheets || [])) {
        let rules;
        try { rules = sheet.cssRules; } catch { continue; }
        if (!rules) continue;
        for (const r of Array.from(rules).slice(0, 600)) {
          if (!r.style) continue;
          for (let i = 0; i < r.style.length; i++) {
            const prop = r.style[i];
            if (prop.startsWith("--")) {
              cssVarCount++;
              const val = r.style.getPropertyValue(prop).trim();
              if (/^#[0-9a-f]{3,6}$|^rgba?\(/i.test(val) && cssVarsExtracted.length < 20) {
                const hx = val.startsWith("#") ? (val.length === 4 ? "#" + val[1]+val[1]+val[2]+val[2]+val[3]+val[3] : val.toLowerCase()) : toHex(val);
                if (hx && /(brand|primary|accent|main|theme|pink|blue)/i.test(prop)) {
                  cssVarsExtracted.push({ hex: hx, var: prop, role: /accent|pink/i.test(prop) ? "accent" : "primary" });
                  addFreq(colorMap, hx);
                }
              }
            }
          }
        }
      }

      // 字体角色推断（display / h1 / heading / body / label）与结构化 text_styles
      const roleQueries = [
        ["display", "h1, [class*='hero' i] h1, [class*='hero' i] h2"],
        ["h1", "h2, [class*='title' i]"],
        ["heading", "h3, [class*='card' i] a, [class*='tit' i]"],
        ["body", "main p, p"],
        ["label", "button, [class*='btn' i], nav a"],
      ];
      const semanticStyles = [];
      const seenRole = new Set();
      const pickRoleStyle = (role, el) => {
        if (!el) return null;
        const cs = getComputedStyle(el);
        const fam = (cs.fontFamily || "").split(",")[0]?.replace(/["']/g, "").trim() || "";
        const sz = Math.round(parseFloat(cs.fontSize) || 14);
        const fw = parseInt(cs.fontWeight, 10) || 400;
        const lhRaw = parseFloat(cs.lineHeight);
        const lh = sz > 0 && Number.isFinite(lhRaw) ? +(lhRaw / sz).toFixed(2) : 1.4;
        const sample = (el.innerText || "").replace(/\s+/g, " ").trim().slice(0, 28);
        return { role, font_family: fam, size: sz, weight: fw, line_height: lh, sample };
      };
      for (const [role, sel] of roleQueries) {
        const el = Array.from(document.querySelectorAll(sel)).find((n) => {
          const r = n.getBoundingClientRect();
          return r.width > 0 && r.height > 0 && (n.innerText || "").trim().length >= 2;
        });
        const st = pickRoleStyle(role, el);
        if (st && !seenRole.has(role)) {
          seenRole.add(role);
          semanticStyles.push(st);
        }
      }
      const h1El = document.querySelector("h1");
      const pEl = document.querySelector("main p, p");
      const pickFont = (el) => {
        if (!el) return null;
        const cs = getComputedStyle(el);
        const fam = (cs.fontFamily || "").split(",")[0]?.replace(/["']/g, "").trim() || "";
        return { family: fam, weight: parseInt(cs.fontWeight, 10) || 400, size: Math.round(parseFloat(cs.fontSize) || 16) };
      };
      const typographyRoles = {
        display: pickFont(h1El),
        body: pickFont(pEl || document.body),
      };

      const palette = [];
      const sortedColors = [...colorMap.entries()].sort((a, b) => (sat(b[0]) - sat(a[0])) || (b[1] - a[1]));
      for (const [hx] of sortedColors) {
        if (sat(hx) >= 24 && palette.every((p) => {
          const dr = Math.abs(parseInt(hx.slice(1,3),16) - parseInt(p.slice(1,3),16));
          const dg = Math.abs(parseInt(hx.slice(3,5),16) - parseInt(p.slice(3,5),16));
          const db = Math.abs(parseInt(hx.slice(5,7),16) - parseInt(p.slice(5,7),16));
          return Math.max(dr, dg, db) > 28;
        })) {
          palette.push(hx);
          if (palette.length >= 4) break;
        }
      }

      // 提取内联 SVG Logo（含 <use href="#..."> symbol 展开）
      let logoSvg = "";
      const logoMarks = [];
      const logoCandidates = Array.from(document.querySelectorAll(
        "header svg, nav svg, a[href='/'] svg, [class*='logo' i] svg, [id*='logo' i] svg"
      ));
      for (const svg of logoCandidates) {
        const r = svg.getBoundingClientRect();
        if (r.width >= 14 && r.height >= 14 && r.width <= 360 && r.height <= 140) {
          const clone = svg.cloneNode(true);
          for (const useEl of clone.querySelectorAll("use")) {
            const ref = useEl.getAttribute("href") || useEl.getAttribute("xlink:href") || "";
            if (ref.startsWith("#")) {
              const target = document.getElementById(ref.slice(1));
              if (target) {
                if (!clone.getAttribute("viewBox") && target.getAttribute("viewBox")) {
                  clone.setAttribute("viewBox", target.getAttribute("viewBox"));
                }
                const wrap = document.createElementNS("http://www.w3.org/2000/svg", "g");
                wrap.innerHTML = target.innerHTML;
                useEl.replaceWith(wrap);
              }
            }
          }
          if (!clone.getAttribute("xmlns")) clone.setAttribute("xmlns", "http://www.w3.org/2000/svg");
          if (!clone.getAttribute("viewBox") && r.width && r.height) {
            clone.setAttribute("viewBox", `0 0 ${Math.round(r.width)} ${Math.round(r.height)}`);
          }
          const html = clone.outerHTML;
          if (html.length > 40 && html.length < 60000) {
            logoSvg = html;
            logoMarks.push({
              kind: r.width > r.height * 1.6 ? "lockup" : "symbol",
              label: "Header Inline SVG",
              inline: true,
            });
            break;
          }
        }
      }

      const logoUrls = [];
      const pushLogoUrl = (u, kind, label) => {
        if (!u || typeof u !== "string" || u.startsWith("data:")) return;
        try {
          const abs = new URL(u, location.href).href;
          if (!logoUrls.includes(abs)) {
            logoUrls.push(abs);
            logoMarks.push({ kind, label, url: abs });
          }
        } catch {}
      };
      for (const img of document.querySelectorAll("header img, nav img, a[href='/'] img, img[alt*='logo' i], img[class*='logo' i]")) {
        const r = img.getBoundingClientRect();
        if (r.width >= 16 && r.height >= 16 && r.width <= 420) {
          pushLogoUrl(img.src, r.width > r.height * 1.6 ? "lockup" : "symbol", img.alt || "Site Logo");
        }
      }
      for (const [sel, kind, lbl] of [
        ['link[rel="apple-touch-icon"]', "symbol", "Apple Touch Icon"],
        ['link[rel="icon"]', "favicon", "Site Favicon"],
        ['link[rel="shortcut icon"]', "favicon", "Shortcut Icon"],
        ['meta[property="og:image"]', "lockup", "OG Social Image"],
      ]) {
        const el = document.querySelector(sel);
        if (el) pushLogoUrl(el.getAttribute("href") || el.getAttribute("content"), kind, lbl);
      }

      const h1 = document.querySelector("h1");
      const heroTitle = (h1 ? h1.innerText : "").replace(/\s+/g, " ").trim().slice(0, 90);
      const metaDesc = (document.querySelector('meta[name="description"]')?.getAttribute("content") ||
        document.querySelector('meta[property="og:description"]')?.getAttribute("content") || "")
        .replace(/\s+/g, " ").trim().slice(0, 160);
      let heroSub = "";
      if (h1 && h1.parentElement) {
        const p = h1.parentElement.querySelector("p");
        if (p) heroSub = p.innerText.replace(/\s+/g, " ").trim().slice(0, 140);
      }
      if (!heroSub) heroSub = metaDesc;

      const bullets = [];
      const seenB = new Set();
      for (const node of document.querySelectorAll("main h2, main h3, section h2, section h3, main li")) {
        const rect = node.getBoundingClientRect();
        if (rect.width <= 0 || rect.height <= 0) continue;
        const tx = (node.innerText || "").replace(/\s+/g, " ").trim();
        if (tx.length >= 6 && tx.length <= 64 && !seenB.has(tx) && tx !== heroTitle) {
          seenB.add(tx);
          bullets.push(tx);
          if (bullets.length >= 5) break;
        }
      }

      const siteTitle = (
        document.querySelector('meta[property="og:site_name"]')?.getAttribute("content") ||
        document.title || ""
      ).replace(/\s+/g, " ").trim().slice(0, 60);

      return {
        site_title: siteTitle,
        background: bg,
        text_color: textColor,
        palette,
        fonts: [...fontMap.entries()].sort((a, b) => b[1] - a[1]).map((x) => x[0]).slice(0, 6),
        typography_roles: typographyRoles,
        text_styles: (semanticStyles.length ? semanticStyles : [...styleMap.entries()]
          .map(([k, count]) => {
            const parts = k.split(":");
            return { role: Number(parts[0]) >= 24 ? "heading" : "body", size: Number(parts[0]), weight: Number(parts[1]), line_height: Number(parts[2]) || 1.4, font_family: parts[3] || "", count };
          })
          .sort((a, b) => (b.size - a.size) || (b.weight - a.weight))
          .slice(0, 6)),
        backgrounds: [...bgMap.entries()].sort((a, b) => b[1] - a[1]).slice(0, 3).map(([hex, count]) => ({ hex, count })),
        shapes: {
          radii: [...radMap.entries()].sort((a, b) => b[1] - a[1]).slice(0, 3).map(([value, count]) => ({ value, count })),
          gaps: [...gapMap.entries()].sort((a, b) => b[1] - a[1]).slice(0, 2).map(([value, count]) => ({ value, count })),
        },
        components: comps,
        css_vars_count: cssVarCount,
        css_var_colors: cssVarsExtracted,
        logo_svg: logoSvg,
        logo_marks: logoMarks.slice(0, 6),
        logos: logoUrls.slice(0, 5),
        site_summary: {
          hero_title: heroTitle || siteTitle,
          hero_subtitle: heroSub,
          description: metaDesc,
          bullets,
        },
        all_colors: [...colorMap.entries()].sort((a, b) => b[1] - a[1]).slice(0, 12).map(([hex, count]) => ({ hex, count })),
        raw_css_bytes: Array.isArray(rawSheets) ? rawSheets.reduce((s, x) => s + x.length, 0) : 0,
        stylesheet_count: Array.isArray(rawSheets) ? rawSheets.length : 0,
      };
    }, sheetTexts);

    // 从捕获到的外部/内联样式表里提取 CSS 变量与色频（解决跨域 CSSOM 限制）
    if (sheetTexts.length) {
      const hay = sheetTexts.join("\n");
      const varRe = /(--[\w-]+)\s*:\s*([^;{}]+)[;}]/g;
      const cssVarMap = new Map();
      let vm;
      while ((vm = varRe.exec(hay)) !== null) {
        const vn = vm[1].toLowerCase();
        if (!cssVarMap.has(vn)) cssVarMap.set(vn, vm[2].trim());
      }
      data.css_vars_count = Math.max(data.css_vars_count || 0, cssVarMap.size);
      const normHex = (raw) => {
        const s = raw.trim().toLowerCase();
        if (/^#[0-9a-f]{6}$/.test(s)) return s;
        if (/^#[0-9a-f]{3}$/.test(s)) return "#" + s[1]+s[1]+s[2]+s[2]+s[3]+s[3];
        const m = s.match(/^rgba?\(\s*(\d+)[,\s]+(\d+)[,\s]+(\d+)(?:[,\s/]+([0-9.]+))?\s*\)$/);
        if (!m || (m[4] !== undefined && parseFloat(m[4]) < 0.15)) return null;
        const h = (n) => Number(n).toString(16).padStart(2, "0");
        return "#" + h(m[1]) + h(m[2]) + h(m[3]);
      };
      const satOf = (hx) => {
        const r = parseInt(hx.slice(1,3),16), g = parseInt(hx.slice(3,5),16), b = parseInt(hx.slice(5,7),16);
        return Math.max(r, g, b) - Math.min(r, g, b);
      };
      const distOf = (a, b) => Math.max(
        Math.abs(parseInt(a.slice(1,3),16)-parseInt(b.slice(1,3),16)),
        Math.abs(parseInt(a.slice(3,5),16)-parseInt(b.slice(3,5),16)),
        Math.abs(parseInt(a.slice(5,7),16)-parseInt(b.slice(5,7),16))
      );
      const roleOf = (vn) => {
        if (/brand|primary|main-color|theme/i.test(vn)) return "primary";
        if (/accent|highlight|active|pink/i.test(vn)) return "accent";
        if (/secondary|second/i.test(vn)) return "secondary";
        if (/bg|background|surface|panel|card/i.test(vn)) return "background";
        if (/text|ink|fg|foreground/i.test(vn)) return "ink";
        return "";
      };
      const varCols = [...(data.css_var_colors || [])];
      const seenVarHex = new Set(varCols.map((c) => c.hex));
      const freq = new Map();
      for (const [vn, val] of cssVarMap.entries()) {
        const hx = normHex(val);
        if (hx) {
          const rl = roleOf(vn);
          if (rl && !seenVarHex.has(hx)) {
            seenVarHex.add(hx);
            varCols.push({ hex: hx, var: vn, role: rl });
          }
          freq.set(hx, (freq.get(hx) || 0) + 2);
        }
      }
      const hexRe = /#[0-9a-fA-F]{3,6}\b|rgba?\([^)]*\)/g;
      let hm;
      while ((hm = hexRe.exec(hay)) !== null) {
        const hx = normHex(hm[0]);
        if (hx) freq.set(hx, (freq.get(hx) || 0) + 1);
      }
      const ranked = [...freq.entries()].sort((a, b) => (satOf(b[0]) - satOf(a[0])) || (b[1] - a[1]));
      const pal = [...(data.palette || [])];
      for (const [hx] of ranked) {
        if (pal.length >= 4) break;
        if (satOf(hx) >= 28 && pal.every((p) => distOf(hx, p) > 32)) pal.push(hx);
      }
      if (pal.length) data.palette = pal;
      data.css_var_colors = varCols.slice(0, 14);
      data.colors_seen = (data.all_colors || []).length + freq.size;
      const fonts = [...(data.fonts || [])];
      const ffRe = /font-family\s*:\s*([^;}]+)/gi;
      let fm;
      while ((fm = ffRe.exec(hay)) !== null) {
        for (const rawFam of fm[1].split(",")) {
          const fam = rawFam.trim().replace(/['"]/g, "").replace(/!important/i, "").trim();
          if (fam && !/^(system-ui|sans-serif|serif|monospace|-apple-system|BlinkMacSystemFont|inherit)$/i.test(fam)) {
            if (!fonts.some((f) => f.toLowerCase() === fam.toLowerCase())) {
              fonts.push(fam);
              if (fonts.length >= 6) break;
            }
          }
        }
        if (fonts.length >= 6) break;
      }
      data.fonts = fonts;
    }

    console.log(JSON.stringify({ ok: true, url: args.url, extra_shots: extraShots, ...data }));
  } finally {
    await browser.close();
  }
}

main().catch((e) => {
  console.error(e && e.message ? e.message : String(e));
  process.exit(1);
});
