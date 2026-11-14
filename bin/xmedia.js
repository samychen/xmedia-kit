#!/usr/bin/env node
// npm bin 壳：转发到 Python 统一 CLI（tools/xmedia.py）。发布为 npm 包
// 后 `npx xmedia-kit render <spec>` 即可用；引擎依赖（FFmpeg/html_bake
// 构建）由 `npx xmedia-kit doctor` 自检指引。
const { spawnSync } = require("child_process");
const path = require("path");
const root = path.resolve(__dirname, "..");
const cli = path.join(root, "tools", "xmedia.py");
const res = spawnSync(process.execPath.startsWith("/usr") ? "python3" : "python3",
                      [cli, ...process.argv.slice(2)],
                      { stdio: "inherit" });
process.exit(res.status ?? 1);
