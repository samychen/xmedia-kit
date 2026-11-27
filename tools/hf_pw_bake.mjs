#!/usr/bin/env node
import { createRequire } from "node:module";
import { spawn } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const { chromium } = require(path.join(
  __dirname, "web", "node_modules", "playwright-core", "index.js"
));

function parseArgs(argv) {
  const out = { fps: 30, width: 1920, height: 1080, seconds: 0 };
  for (let i = 2; i < argv.length; i++) {
    const k = argv[i], v = argv[i + 1];
    if (k === "--url") { out.url = v; i++; }
    else if (k === "--out") { out.out = v; i++; }
    else if (k === "--seconds") { out.seconds = parseFloat(v) || 0; i++; }
    else if (k === "--fps") { out.fps = parseInt(v, 10) || 30; i++; }
    else if (k === "--width") { out.width = parseInt(v, 10) || 1920; i++; }
    else if (k === "--height") { out.height = parseInt(v, 10) || 1080; i++; }
  }
  return out;
}

async function launchBrowser() {
  for (const opts of [
    { headless: true },
    { headless: true, channel: "chrome" },
    { headless: true, executablePath: "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" },
  ]) {
    try { return await chromium.launch(opts); } catch (_) {}
  }
  throw new Error("未找到可用的 Chromium / Google Chrome 浏览器内核");
}

async function main() {
  const args = parseArgs(process.argv);
  if (!args.url || !args.out) {
    throw new Error("缺少 --url 或 --out 参数");
  }
  if (args.seconds <= 0) {
    throw new Error("视频总时长为 0s，请至少保留 1 个场景后再导出");
  }
  const totalFrames = Math.max(1, Math.round(args.seconds * args.fps));
  const browser = await launchBrowser();
  let ff = null;
  try {
    const page = await browser.newPage({
      viewport: { width: args.width, height: args.height },
      deviceScaleFactor: 1,
    });
    await page.goto(args.url, { waitUntil: "load" });
    await page.evaluate(async () => {
      if (document.fonts && document.fonts.ready) await document.fonts.ready;
    });

    const meta = await page.evaluate(({ targetW, targetH }) => {
      const root = document.getElementById("root");
      const rw = Number(root?.dataset.width || targetW);
      const rh = Number(root?.dataset.height || targetH);
      if (root && (rw !== targetW || rh !== targetH)) {
        const scale = Math.min(targetW / rw, targetH / rh);
        root.style.transformOrigin = "top left";
        root.style.transform = `scale(${scale})`;
      }
      const audios = Array.from(document.querySelectorAll("audio")).map((el) => ({
        src: el.getAttribute("src") || "",
        start: Number(el.dataset.start || 0),
        duration: Number(el.dataset.duration || 0),
        volume: Number(el.dataset.volume || 1),
      }));
      return { audios };
    }, { targetW: args.width, targetH: args.height });

    const baseDir = args.url.startsWith("file://")
      ? path.dirname(decodeURIComponent(args.url.slice(7)))
      : process.cwd();
    const audioInputs = [];
    for (const a of meta.audios) {
      if (!a.src) continue;
      const resolved = path.isAbsolute(a.src) ? a.src : path.resolve(baseDir, a.src);
      if (fs.existsSync(resolved)) audioInputs.push({ ...a, file: resolved });
    }

    const ffArgs = ["-y", "-f", "image2pipe", "-vcodec", "mjpeg",
      "-framerate", String(args.fps), "-i", "-"];
    for (const a of audioInputs) {
      ffArgs.push("-i", a.file);
    }

    if (audioInputs.length > 0) {
      const filters = [];
      const mixLabels = [];
      audioInputs.forEach((a, idx) => {
        const inIdx = idx + 1;
        const delayMs = Math.max(0, Math.round(a.start * 1000));
        const trimPart = a.duration > 0 ? `atrim=0:${a.duration},` : "";
        filters.push(`[${inIdx}:a]${trimPart}volume=${a.volume},adelay=${delayMs}|${delayMs}[a${idx}]`);
        mixLabels.push(`[a${idx}]`);
      });
      filters.push(`${mixLabels.join("")}amix=inputs=${audioInputs.length}:duration=longest:normalize=0[aout]`);
      ffArgs.push("-filter_complex", filters.join(";"), "-map", "0:v", "-map", "[aout]");
    } else {
      ffArgs.push("-map", "0:v");
    }

    const ext = path.extname(args.out).toLowerCase();
    if (ext === ".webm") {
      ffArgs.push("-c:v", "libvpx-vp9", "-crf", "30", "-b:v", "0", "-pix_fmt", "yuv420p");
      if (audioInputs.length > 0) ffArgs.push("-c:a", "libopus");
    } else {
      ffArgs.push("-c:v", "libx264", "-preset", "veryfast", "-crf", "18",
        "-pix_fmt", "yuv420p", "-movflags", "+faststart");
      if (audioInputs.length > 0) ffArgs.push("-c:a", "aac", "-b:a", "192k");
    }
    ffArgs.push("-t", String(args.seconds), args.out);

    ff = spawn("ffmpeg", ffArgs, { stdio: ["pipe", "pipe", "pipe"] });
    let ffErr = "";
    ff.stderr.on("data", (d) => { ffErr += d.toString().slice(-1000); });
    const ffDone = new Promise((resolve, reject) => {
      ff.on("error", reject);
      ff.on("close", (code) => {
        if (code === 0) resolve();
        else reject(new Error(`ffmpeg exit ${code}: ${ffErr.slice(-400)}`));
      });
    });

    for (let f = 0; f < totalFrames; f++) {
      const t = f / args.fps;
      await page.evaluate(async (timeSec) => {
        const tls = Object.values(window.__timelines || {});
        for (const tl of tls) tl.seek(timeSec, false);
        const vids = document.querySelectorAll("video");
        for (const v of vids) {
          const s = Number(v.dataset.start || 0);
          const d = Number(v.dataset.duration || 0);
          const mStart = Number(v.dataset.mediaStart || 0);
          if (timeSec >= s && timeSec <= s + d) {
            const target = mStart + (timeSec - s);
            if (Math.abs(v.currentTime - target) > 0.04) v.currentTime = target;
          }
        }
        await new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
      }, t);
      const buf = await page.screenshot({ type: "jpeg", quality: 92 });
      if (!ff.stdin.write(buf)) {
        await new Promise((r) => ff.stdin.once("drain", r));
      }
    }
    ff.stdin.end();
    await ffDone;
  } finally {
    await browser.close();
  }
}

main().catch((err) => {
  console.error(err?.message || String(err));
  process.exit(1);
});
