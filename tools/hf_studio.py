#!/usr/bin/env python3
# xmedia Studio：契约 HTML 的预览与编排（Player-lite + 本地服务）。
#
# 两种模式：
#   1. 注入模式（默认）：hf_studio.py <contract.html> [-o studio.html]
#      向契约页注入传输条（播放/拖动/场景跳转）+ 驱动脚本，浏览器打开
#      即预览。单文件自包含；契约页语义不变；幂等可原地更新。
#   2. 服务模式：hf_studio.py <contract.html> --serve [--spec spec.json]
#      [--port 8120]——本地 HTTP 服务 + 浏览器 UI（对位 Remotion Studio
#      的核心闭环：spec 编辑器热重组、TTS 一键、场景时间轴、一键烘焙，
#      参数从 spec 自动推导）。仅绑定 127.0.0.1。
#
# 驱动与烘焙 seek 语义同源：__timelines totalTime（秒制）+
# __mcApplyTimeline（毫秒可见性）。注入的 chrome DOM 不携带
# data-start/duration——避免被可见性切换与 hf_validate 误伤。幂等：
# 重复注入剥离旧 chrome 再重注。

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TOOLS_DIR)
MARK_BEGIN = "<!-- xmedia-kit studio:begin -->"
MARK_END = "<!-- xmedia-kit studio:end -->"
AUDIO_EXTS = {".wav", ".mp3", ".m4a", ".aac"}
FILE_EXTS = AUDIO_EXTS | {".txt", ".json", ".html"}

sys.path.insert(0, TOOLS_DIR)


def parse_scenes(html):
    """契约页的场景清单：div 且同时带 data-start/data-duration——合成根
    （data-composition-id）是容器不是场景，排除。
    返回 [{id, start, dur, label}]（秒）。"""
    scenes = []
    for m in re.finditer(r'<div\b([^>]*)>', html, re.S):
        attrs = m.group(1)
        if "data-composition-id" in attrs:
            continue
        start = re.search(r'data-start="([\d.]+)"', attrs)
        dur = re.search(r'data-duration="([\d.]+)"', attrs)
        sid_m = re.search(r'id="([^"]+)"', attrs)
        cls_m = re.search(r'class="([^"]*)"', attrs)
        if not (start and dur and sid_m):
            continue
        sid = sid_m.group(1)
        cls = cls_m.group(1) if cls_m else ""
        typ = next((c for c in cls.split() if c.startswith("scene-")
                    and c != "scene"), sid)
        scenes.append({"id": sid, "start": float(start.group(1)),
                       "dur": float(dur.group(1)), "label": typ})
    return scenes


def parse_audio_tracks(html):
    """页面 <audio> 元素 → 音轨清单 [{src, start, dur, vol, kind}]（秒）。
    kind：narration（voice/ 旁白）/ sfx（assets/sfx）/ bgm（其余）。"""
    tracks = []
    for m in re.finditer(r'<audio\b([^>]*)>', html, re.S):
        attrs = m.group(1)
        src = re.search(r'src="([^"]+)"', attrs)
        start = re.search(r'data-start="([\d.]+)"', attrs)
        dur = re.search(r'data-duration="([\d.]+)"', attrs)
        vol = re.search(r'data-volume="([\d.]+)"', attrs)
        if not (src and start):
            continue
        s = src.group(1)
        kind = ("sfx" if "assets/sfx" in s else
                "narration" if ("voice/" in s or "narr" in
                                os.path.basename(s)) else "bgm")
        tracks.append({"src": s, "start": float(start.group(1)),
                       "dur": float(dur.group(1)) if dur else 0.0,
                       "vol": float(vol.group(1)) if vol else 1.0,
                       "kind": kind})
    return tracks


def parse_total(html, scenes):
    """总时长（秒）：合成根 data-duration 优先，回退场景窗最大 end。"""
    m = re.search(r'data-composition-id[^>]*data-duration="([\d.]+)"',
                  html, re.S)
    if m:
        return float(m.group(1))
    return max((s["start"] + s["dur"] for s in scenes), default=0.0)


def build_chrome(total_s, scenes):
    """注入块：注释标记包裹的 chrome DOM + 驱动脚本。"""
    scenes_json = json.dumps(scenes, ensure_ascii=False)
    return f'''{MARK_BEGIN}
<style>
  #__mc-studio-chrome {{
    position: fixed; bottom: 14px; left: 50%; transform: translateX(-50%);
    height: 44px; padding: 0 14px; border-radius: 22px;
    background: rgba(10, 10, 16, 0.82); backdrop-filter: blur(10px);
    border: 1px solid rgba(255, 255, 255, 0.14); color: #e8e8f0;
    display: flex; align-items: center; gap: 10px;
    font: 13px/1 -apple-system, "Helvetica Neue", sans-serif;
    z-index: 2147483647; user-select: none;
    box-shadow: 0 6px 24px rgba(0, 0, 0, 0.45);
  }}
  #__mc-studio-chrome button {{
    background: rgba(255, 255, 255, 0.08); color: #e8e8f0; border: 0;
    border-radius: 14px; padding: 6px 11px; cursor: pointer; font: inherit;
  }}
  #__mc-studio-chrome button:hover {{ background: rgba(255, 255, 255, 0.16); }}
  #__mc-studio-bar {{ width: 240px; accent-color: #8ba0ff; }}
  #__mc-studio-scenes {{ display: flex; gap: 5px; max-width: 42vw;
    overflow-x: auto; }}
  #__mc-studio-scenes button {{ padding: 5px 9px; font-size: 12px;
    white-space: nowrap; }}
  #__mc-studio-time {{ min-width: 92px; text-align: right;
    font-variant-numeric: tabular-nums; }}
</style>
<div id="__mc-studio-chrome">
  <button id="__mc-studio-play">▶</button>
  <input id="__mc-studio-bar" type="range" min="0" max="1000" value="0">
  <span id="__mc-studio-time">0.0 / {total_s:.1f}s</span>
  <div id="__mc-studio-scenes">
    <button data-goto="0">⏮</button>
{"".join(f'    <button data-goto="{s["start"]}">{s["label"]}</button>'
         for s in scenes)}
  </div>
</div>
<script>
(function() {{
  if (window.__mcStudio) {{ return; }}
  window.__mcStudio = true;
  var TOTAL = {total_s};
  var SCENES = {scenes_json};
  var playing = false, t = 0, raf = 0, last = 0;
  var bar = document.getElementById("__mc-studio-bar");
  var btn = document.getElementById("__mc-studio-play");
  var label = document.getElementById("__mc-studio-time");

  function drive(sec) {{
    // 与烘焙 seek 语义同源：注册表 totalTime（秒）+ 毫秒可见性。
    Object.values(window.__timelines || {{}}).forEach(function (tl) {{
      try {{
        if (typeof tl.pause === "function") {{ tl.pause(); }}
        if (typeof tl.totalTime === "function") {{
          tl.totalTime(sec + 1e-3, true);
        }} else if (typeof tl.seek === "function") {{ tl.seek(sec); }}
      }} catch (e) {{}}
    }});
    if (typeof window.__mcApplyTimeline === "function") {{
      try {{ window.__mcApplyTimeline(sec * 1000); }} catch (e) {{}}
    }} else if (typeof window.seek === "function") {{
      try {{ window.seek(sec * 1000); }} catch (e) {{}}
    }}
    // 视频元素同步到时间轴位置（scrub 精确；播放态逐帧 seek）。
    try {{
      var vids = document.querySelectorAll("video");
      for (var vi = 0; vi < vids.length; vi++) {{
        var v = vids[vi];
        var vs = parseFloat(v.getAttribute("data-start") || "0");
        var local = Math.max(0, sec - vs);
        if (Math.abs(v.currentTime - local) > 0.05) {{
          v.currentTime = local;
        }}
      }}
    }} catch (e) {{}}
    bar.value = TOTAL > 0 ? Math.round(sec / TOTAL * 1000) : 0;
    label.textContent = sec.toFixed(1) + " / " + TOTAL.toFixed(1) + "s";
    // Studio 服务模式：把播放时间回报给父页（多轨时间轴播放头联动）。
    if (window.parent !== window) {{
      try {{ window.parent.postMessage({{ mcStudioTime: sec, total: TOTAL }},
        "*"); }} catch (e) {{}}
    }}
  }}
  function frame(now) {{
    if (!playing) {{ return; }}
    var dt = (now - last) / 1000; last = now;
    t = Math.min(TOTAL, t + dt);
    drive(t);
    if (t >= TOTAL) {{
      playing = false; btn.textContent = "▶";
      if (window.__mcStudioLoop) {{ t = 0; playing = true; last = now;
        raf = requestAnimationFrame(frame); }}
      return;
    }}
    raf = requestAnimationFrame(frame);
  }}
  function play() {{
    if (playing) {{ return; }}
    if (t >= TOTAL) {{ t = 0; }}
    playing = true; last = performance.now();
    btn.textContent = "⏸";
    raf = requestAnimationFrame(frame);
  }}
  function pause() {{
    playing = false; btn.textContent = "▶";
    if (raf) {{ cancelAnimationFrame(raf); raf = 0; }}
  }}
  btn.addEventListener("click", function () {{
    playing ? pause() : play();
  }});
  bar.addEventListener("input", function () {{
    pause();
    t = TOTAL * (parseFloat(bar.value) / 1000);
    drive(t);
  }});
  document.getElementById("__mc-studio-scenes")
    .addEventListener("click", function (ev) {{
      var g = ev.target.getAttribute && ev.target.getAttribute("data-goto");
      if (g === null || g === undefined) {{ return; }}
      pause(); t = parseFloat(g); drive(t);
    }});
  // Studio 服务的时间轴联动：父页面 postMessage {{mcStudioSeek: 秒}}。
  window.addEventListener("message", function (ev) {{
    var d = ev.data || {{}};
    if (typeof d.mcStudioSeek === "number") {{
      pause(); t = Math.max(0, Math.min(TOTAL, d.mcStudioSeek)); drive(t);
    }}
  }});
  document.addEventListener("keydown", function (ev) {{
    if (ev.code === "Space" && ev.target === document.body) {{
      ev.preventDefault(); playing ? pause() : play();
    }} else if (ev.code === "ArrowLeft") {{
      pause(); t = Math.max(0, t - 1); drive(t);
    }} else if (ev.code === "ArrowRight") {{
      pause(); t = Math.min(TOTAL, t + 1); drive(t);
    }}
  }});
  drive(0);
}})();
</script>
{MARK_END}'''


def inject(html):
    """幂等注入：已存在 chrome 则先剥离旧块再重注入（spec 变更后可重生成）。"""
    if MARK_BEGIN in html:
        html = re.sub(re.escape(MARK_BEGIN) + r"[\s\S]*?" + re.escape(MARK_END),
                      "", html)
    scenes = parse_scenes(html)
    total = parse_total(html, scenes)
    chrome = build_chrome(total, scenes)
    if "</body>" in html:
        return html.replace("</body>", chrome + "\n</body>", 1)
    return html + "\n" + chrome


# ---------------------------------------------------------------------------
# 服务模式（xmedia Studio：本地服务 + 浏览器 UI）
# ---------------------------------------------------------------------------

STUDIO_UI = r"""<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>xmedia Studio</title>
<style>
  :root {
    --bg: #0b0b11; --panel: #15151d; --panel2: #1a1a24; --line: #26263a;
    --ink: #e9e9f0; --dim: #9494a8; --acc: #6d8bff; --acc2: #8b6dff;
    --ok: #3ecf8e; --err: #ff5c6c; --warn: #e8b64c;
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--ink);
         font: 14px/1.45 -apple-system, "Helvetica Neue", sans-serif;
         height: 100vh; display: flex; flex-direction: column; overflow: hidden; }
  header { display: flex; align-items: center; gap: 12px; height: 46px;
           padding: 0 14px; border-bottom: 1px solid var(--line);
           background: var(--panel); flex: none; }
  .logo { width: 24px; height: 24px; border-radius: 7px;
          background: linear-gradient(135deg, var(--acc), var(--acc2)); }
  header b { font-size: 13px; letter-spacing: 0.3px; }
  header .file { color: var(--dim); font-size: 12px; background: var(--panel2);
                 padding: 4px 10px; border-radius: 6px; }
  header .sp { flex: 1; }
  header button { background: var(--panel2); color: var(--ink); border: 1px
                  solid var(--line); border-radius: 7px; padding: 7px 13px;
                  cursor: pointer; font: inherit; font-size: 13px; }
  header button:hover { border-color: var(--acc); }
  header button.acc { background: linear-gradient(135deg, var(--acc),
                       var(--acc2)); border: 0; color: #0d1122;
                       font-weight: 600; }
  header button:disabled { opacity: 0.4; cursor: default; }
  main { flex: 1; display: grid; grid-template-columns: 420px 1fr;
         min-height: 0; }
  #left { border-right: 1px solid var(--line); display: flex;
          flex-direction: column; min-height: 0; background: var(--panel); }
  .panel-h { height: 34px; display: flex; align-items: center; gap: 8px;
             padding: 0 14px; border-bottom: 1px solid var(--line);
             font-size: 11px; letter-spacing: 0.8px; color: var(--dim);
             text-transform: uppercase; flex: none; }
  #spec { flex: 1; width: 100%; background: transparent; color: var(--ink);
          border: 0; outline: 0; resize: none; padding: 10px 14px;
          font: 12px/1.6 ui-monospace, Menlo, monospace; }
  #right { display: flex; flex-direction: column; min-height: 0;
           background: var(--bg); }
  #pvwrap { flex: 1; display: flex; align-items: center;
            justify-content: center; min-height: 0; padding: 18px;
            background: repeating-conic-gradient(#12121a 0 25%,
                         #0e0e14 0 50%) 0 0/22px 22px; }
  #pvframe { position: relative; background: #000; border-radius: 12px;
             border: 1px solid var(--line); overflow: hidden;
             box-shadow: 0 10px 44px rgba(0, 0, 0, 0.55); }
  #pvframe iframe { width: 100%; height: 100%; border: 0; display: block; }
  #scene-badge { position: absolute; top: 10px; left: 10px; font-size: 11px;
                 color: var(--dim); background: rgba(10, 10, 16, 0.7);
                 padding: 4px 9px; border-radius: 6px; z-index: 2147483000; }
  #findings { flex: none; max-height: 96px; overflow: auto; padding: 8px 14px;
              border-top: 1px solid var(--line); background: var(--panel); }
  #findings:empty { display: none; }
  #findings .f { font-size: 12px; padding: 1px 0; }
  #findings .f.err { color: var(--err); }
  #findings .f.warn { color: var(--warn); }
  #findings .f.info { color: var(--dim); }
  #bottom { border-top: 1px solid var(--line); height: 248px; flex: none;
            display: flex; flex-direction: column; background: var(--panel); }
  #tl-head { display: flex; align-items: center; gap: 10px; height: 32px;
             padding: 0 14px; flex: none; }
  #tl-head .cap { font-size: 11px; letter-spacing: 0.8px; color: var(--dim);
                  text-transform: uppercase; }
  #tlabel { font-variant-numeric: tabular-nums; color: var(--ink);
            background: var(--panel2); padding: 3px 9px; border-radius: 6px; }
  #tl-head .hint { color: var(--dim); }
  #timeline { position: relative; flex: 1; overflow: hidden;
              padding: 0 14px 8px; cursor: crosshair; }
  .tl-ruler { position: relative; height: 16px; margin-left: 58px;
              font-size: 10px; color: var(--dim); }
  .tl-ruler .tick { position: absolute; top: 0; height: 100%;
                    border-left: 1px solid #2e2e42; padding-left: 3px; }
  .tl-row { display: flex; height: 30px; margin-top: 3px; }
  .tl-label { width: 58px; flex: none; font-size: 11px; color: var(--dim);
              display: flex; align-items: center; }
  .tl-lane { flex: 1; position: relative; background: #12121a;
             border-radius: 5px; }
  .tl-blk { position: absolute; top: 3px; bottom: 3px; border-radius: 5px;
            cursor: pointer; overflow: hidden; font-size: 11px;
            padding: 4px 7px; white-space: nowrap; color: var(--ink); }
  .tl-blk.scene { background: linear-gradient(180deg, #2b3560, #232b4c);
                  border: 1px solid #46528c; }
  .tl-blk.narration { background: linear-gradient(180deg, #3a2d5c, #2e244a);
                      border: 1px solid #64468f; }
  .tl-blk.sfx { background: linear-gradient(180deg, #24443a, #1c362f);
                border: 1px solid #3f7a64; }
  .tl-blk.bgm { background: linear-gradient(180deg, #4c3f24, #3a3020);
                border: 1px solid #8f7a46; }
  .tl-blk:hover { filter: brightness(1.25); }
  .tl-blk.sel { border-color: var(--acc);
                box-shadow: 0 0 0 1px var(--acc) inset; }
  .tl-blk.locked { cursor: default; }
  .tl-blk .dur { color: var(--dim); margin-left: 6px;
                 font-variant-numeric: tabular-nums; }
  .tl-blk .trim { position: absolute; right: 0; top: 0; bottom: 0;
                  width: 9px; cursor: ew-resize;
                  background: rgba(255, 255, 255, 0.12); }
  .tl-blk .trim:hover { background: rgba(108, 139, 255, 0.55); }
  .tl-blk .mv { position: absolute; top: 0; bottom: 0; width: 15px;
                cursor: pointer; color: var(--ink); font-size: 12px;
                display: flex; align-items: center; justify-content: center;
                background: rgba(11, 11, 17, 0.72); }
  .tl-blk .mv.left { left: 0; }
  .tl-blk .mv.right { right: 9px; }
  #playhead { position: absolute; top: 0; bottom: 0; width: 0;
              pointer-events: none; z-index: 9; }
  #playhead::before { content: ""; position: absolute; top: 0; bottom: 0;
                      left: -1px; width: 2px; background: #ff5c7a; }
  #playhead::after { content: ""; position: absolute; top: -1px; left: -6px;
                     border: 6px solid transparent; border-top: 7px solid
                     #ff5c7a; }
  #log { flex: 1; overflow: auto; padding: 6px 14px; font: 11px/1.55
         ui-monospace, Menlo, monospace; color: var(--dim);
         border-top: 1px solid var(--line); white-space: pre-wrap; }
  #log .err { color: var(--err); }
  #log .ok { color: var(--ok); }
  .spin { display: inline-block; width: 12px; height: 12px;
          border: 2px solid var(--dim); border-top-color: var(--acc);
          border-radius: 50%; animation: sp 0.8s linear infinite; }
  @keyframes sp { to { transform: rotate(360deg); } }
</style>
</head>
<body>
<header>
  <div class="logo"></div>
  <b>xmedia Studio</b>
  <span class="file" id="fileinfo"></span>
  <span class="sp"></span>
  <button id="btn-tts" title="对 spec 全场景生成旁白（say/HTTP）">生成旁白</button>
  <button id="btn-recompose" class="acc" title="Ctrl/Cmd+Enter">重组合成</button>
  <button id="btn-bake">烘焙 MP4</button>
  <span id="job" style="min-width:70px;font-size:12px;color:var(--dim)"></span>
</header>
<main>
  <div id="left">
    <div class="panel-h">SPEC JSON
      <span style="flex:1"></span>
      <label style="display:flex;align-items:center;gap:5px;cursor:pointer;
                    text-transform:none;letter-spacing:0">
        <input type="checkbox" id="sfx" checked> SFX</label>
      <span style="text-transform:none;letter-spacing:0">Ctrl/Cmd+Enter 重组</span>
    </div>
    <textarea id="spec" spellcheck="false"></textarea>
  </div>
  <div id="right">
    <div id="pvwrap">
      <div id="pvframe">
        <iframe id="pv"></iframe>
        <div id="scene-badge"></div>
      </div>
    </div>
    <div id="findings"></div>
  </div>
</main>
<div id="bottom">
  <div id="tl-head">
    <span class="cap">时间轴</span>
    <span id="tlabel">0.0s</span>
    <span class="hint">场景块：点选 ‹ › 排序 · 拖右缘裁剪（旁白锁定）·
      标尺点击/拖动定位 · 空格播放</span>
  </div>
  <div id="timeline"></div>
  <div id="log"></div>
</div>
<script>
var state = { spec: null, duration: 0, scenes: [], audios: [],
              width: 1080, height: 1920 };
var selIdx = -1;
var $ = function (id) { return document.getElementById(id); };
function log(msg, cls) {
  var el = document.createElement("div");
  if (cls) el.className = cls;
  el.textContent = msg;
  $("log").appendChild(el);
  $("log").scrollTop = $("log").scrollHeight;
}
function api(path, body) {
  return fetch(path, { method: body ? "POST" : "GET",
    headers: { "Content-Type": "application/json" },
    body: body ? JSON.stringify(body) : undefined }).then(function (r) {
      return r.json().then(function (j) {
        if (j.error) throw new Error(j.error);
        return j;
      });
    });
}
function activeScene(t) {
  for (var i = 0; i < state.scenes.length; i++) {
    var s = state.scenes[i];
    if (t >= s.start && t < s.start + s.dur) return s;
  }
  return null;
}
function renderTimeline() {
  var tl = $("timeline"); tl.innerHTML = "";
  var total = Math.max(state.duration, 0.1);
  var ruler = document.createElement("div"); ruler.className = "tl-ruler";
  var step = total > 24 ? 5 : total > 12 ? 2 : 1;
  for (var s = 0; s <= total; s += step) {
    var tk = document.createElement("div"); tk.className = "tick";
    tk.style.left = (s / total * 100) + "%";
    tk.textContent = s + "s";
    ruler.appendChild(tk);
  }
  tl.appendChild(ruler);
  var phWrap = document.createElement("div");
  phWrap.style.position = "relative";
  var ph = document.createElement("div"); ph.id = "playhead";
  phWrap.appendChild(ph);
  [["场景", "scene"], ["旁白", "narration"], ["SFX", "sfx"],
   ["音乐", "bgm"]].forEach(function (ld) {
    var row = document.createElement("div"); row.className = "tl-row";
    phWrap.appendChild(row);
    var lab = document.createElement("div"); lab.className = "tl-label";
    lab.textContent = ld[0];
    var lane = document.createElement("div");
    lane.className = "tl-lane " + ld[1];
    lane.dataset.kind = ld[1];
    row.appendChild(lab); row.appendChild(lane);
    if (ld[1] === "scene") {
      state.scenes.forEach(function (s, idx) {
        var locked = !!(state.spec.scenes[idx] &&
                        state.spec.scenes[idx].narration);
        var b = document.createElement("div");
        b.className = "tl-blk" + (locked ? " locked" : "") +
                      (idx === selIdx ? " sel" : "");
        b.style.left = (s.start / total * 100) + "%";
        b.style.width = Math.max(1.5, s.dur / total * 100) + "%";
        b.innerHTML = s.label +
            '<span class="dur">' + s.dur.toFixed(1) + "s</span>";
        b.title = locked ? "旁白锁定：时长随旁白拟合"
                         : "点选：‹ › 排序；拖右缘裁剪时长";
        b.onclick = function (ev) {
          if (ev.target.className.indexOf("trim") >= 0 ||
              ev.target.className.indexOf("mv") >= 0) { return; }
          selIdx = (selIdx === idx) ? -1 : idx;
          renderTimeline();
        };
        if (!locked) {
          var trim = document.createElement("div"); trim.className = "trim";
          trim.onmousedown = function (e) {
            e.stopPropagation();
            var px0 = e.clientX, d0 = s.dur;
            function mv(e2) {
              var d = d0 + (e2.clientX - px0) / (lane.clientWidth) * total;
              d = Math.max(0.3, Math.min(d, total - s.start));
              b.style.width = Math.max(1.5, d / total * 100) + "%";
              b._newDur = d;
            }
            function up() {
              document.removeEventListener("mousemove", mv);
              document.removeEventListener("mouseup", up);
              if (b._newDur && Math.abs(b._newDur - d0) > 0.05) {
                trimCommit(idx, b._newDur);
              }
            }
            document.addEventListener("mousemove", mv);
            document.addEventListener("mouseup", up);
          };
          b.appendChild(trim);
        }
        if (idx === selIdx && !locked) {
          var mvL = document.createElement("div"); mvL.className = "mv left";
          mvL.textContent = "‹"; mvL.title = "前移一位";
          mvL.onclick = function (e) { e.stopPropagation(); reorder(idx, -1); };
          var mvR = document.createElement("div"); mvR.className = "mv right";
          mvR.textContent = "›"; mvR.title = "后移一位";
          mvR.onclick = function (e) { e.stopPropagation(); reorder(idx, 1); };
          b.appendChild(mvL); b.appendChild(mvR);
        }
        lane.appendChild(b);
      });
    } else {
      state.audios.filter(function (a) { return a.kind === ld[1]; })
        .forEach(function (a) {
          var b = document.createElement("div");
          b.className = "tl-blk locked";
          b.style.left = (a.start / total * 100) + "%";
          b.style.width = Math.max(1.5,
              Math.min(a.dur || total, total) / total * 100) + "%";
          var nm = a.src.split("/").pop();
          b.innerHTML = nm + '<span class="dur">v' + a.vol.toFixed(1) +
                        "</span>";
          b.title = a.src;
          b.onclick = function () { seekTo(a.start + 0.01); };
          lane.appendChild(b);
        });
    }
  });
  $("timeline").appendChild(phWrap);
  updatePlayhead(lastTime, total);
}
function trimCommit(idx, newDur) {
  state.spec.scenes[idx].duration = Math.round(newDur * 100) / 100;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("裁剪 scene-" + idx + " → " + state.spec.scenes[idx].duration +
      "s，重组合成…");
  recompose();
}
function reorder(idx, dir) {
  var arr = state.spec.scenes, j = idx + dir;
  if (j < 0 || j >= arr.length) { return; }
  var t = arr[idx]; arr[idx] = arr[j]; arr[j] = t;
  selIdx = j;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("场景 " + idx + " " + (dir < 0 ? "前移" : "后移") + "，重组合成…");
  recompose();
}
function seekTo(sec) {
  $("pv").contentWindow.postMessage({ mcStudioSeek: sec }, "*");
}
var lastTime = 0;
function updatePlayhead(t, total) {
  lastTime = t;
  var pct = (total > 0 ? Math.min(t, total) / total : 0) * 100 + "%";
  var ph = $("playhead"); if (ph) ph.style.left = "calc(58px + " + pct +
      " * (100% - 58px))";
  $("tlabel").textContent = t.toFixed(1) + "s / " + total.toFixed(1) + "s";
  var badge = $("scene-badge");
  var sc = activeScene(t);
  if (badge) badge.textContent = sc ? sc.label : "—";
}
window.addEventListener("message", function (ev) {
  var d = ev.data || {};
  if (typeof d.mcStudioTime === "number") {
    updatePlayhead(d.mcStudioTime, d.total || state.duration);
  }
});
// 标尺点击/拖动定位（跨四轨的统一 scrub）
(function () {
  var scrub = function (ev) {
    var tl = $("timeline");
    var rect = tl.getBoundingClientRect();
    var laneX = ev.clientX - rect.left - 58;
    var laneW = rect.width - 58 - 14;
    if (laneW <= 0 || laneX < 0) { return; }
    var total = Math.max(state.duration, 0.1);
    seekTo(Math.max(0, Math.min(total, laneX / laneW * total)));
  };
  $("timeline").addEventListener("mousedown", function (ev) {
    if (ev.target.closest(".tl-blk")) { return; }
    scrub(ev);
    function mv(e2) { scrub(e2); }
    function up() {
      document.removeEventListener("mousemove", mv);
      document.removeEventListener("mouseup", up);
    }
    document.addEventListener("mousemove", mv);
    document.addEventListener("mouseup", up);
  });
})();
function renderFindings(f) {
  var el = $("findings"); el.innerHTML = "";
  (f || []).forEach(function (x) {
    var d = document.createElement("div");
    d.className = "f " + x.level;
    d.textContent = "[" + x.level + "] " + x.message;
    el.appendChild(d);
  });
}
function fitPreview() {
  var wrap = $("pvwrap"), fr = $("pvframe");
  var aw = state.width / state.height;
  var maxW = Math.max(160, wrap.clientWidth - 40);
  var maxH = Math.max(160, wrap.clientHeight - 40);
  var w = Math.min(maxW, maxH * aw);
  fr.style.width = w + "px";
  fr.style.height = (w / aw) + "px";
  $("pv").style.width = "100%";
  $("pv").style.height = "100%";
}
function refresh(keepScroll) {
  return api("/api/state").then(function (s) {
    state = s;
    if (!keepScroll) $("spec").value = JSON.stringify(s.spec, null, 2);
    $("sfx").checked = !(s.spec.sfx && s.spec.sfx.enabled === false);
    $("fileinfo").textContent = (s.spec_path || "未绑定 spec").split("/").pop();
    fitPreview(); renderTimeline(); renderFindings(s.findings);
    $("pv").src = "/preview?t=" + Date.now();
  });
}
function recompose() {
  var spec;
  try { spec = JSON.parse($("spec").value); }
  catch (e) { log("spec JSON 解析失败：" + e.message, "err"); return; }
  spec.sfx = { enabled: $("sfx").checked };
  api("/api/recompose", { spec: spec, no_sfx: !$("sfx").checked })
    .then(function (r) {
      return refresh(true).then(function () {
        log((r.ok ? "重组成功 " : "重组完成（含校验错误）") +
            r.duration.toFixed(1) + "s / " + r.scenes.length + " 场景 / sha " +
            r.sha, r.ok ? "ok" : "err");
        if (!r.ok) log("hf_validate 未过 0 errors——预览可用，烘焙会被拒绝",
                       "err");
      });
    })
    .catch(function (e) { log("重组失败：" + e.message, "err"); });
}
$("btn-recompose").onclick = recompose;
document.addEventListener("keydown", function (ev) {
  if ((ev.metaKey || ev.ctrlKey) && ev.key === "Enter") {
    ev.preventDefault(); recompose();
  }
});
$("sfx").addEventListener("change", recompose);
$("btn-tts").onclick = function () {
  var spec;
  try { spec = JSON.parse($("spec").value); }
  catch (e) { log("spec JSON 解析失败：" + e.message, "err"); return; }
  $("btn-tts").disabled = true;
  api("/api/tts", { spec: spec }).then(function (r) {
    $("btn-tts").disabled = false;
    $("spec").value = JSON.stringify(r.spec, null, 2);
    log("旁白生成完成：" + r.generated + " 段（voice/ 目录）", "ok");
    recompose();
  }).catch(function (e) {
    $("btn-tts").disabled = false;
    log("TTS 失败：" + e.message, "err");
  });
};
$("btn-bake").onclick = function () {
  $("btn-bake").disabled = true;
  $("job").innerHTML = '<span class="spin"></span> 烘焙中';
  log("烘焙开始（参数由 spec 自动推导：" + state.width + "×" + state.height +
      " / " + state.duration.toFixed(1) + "s / " +
      (state.spec.composition || {}).fps + "fps）");
  api("/api/bake", {}).then(function (r) { return pollJob(r.job); })
    .then(function (j) {
      $("btn-bake").disabled = false; $("job").textContent = "";
      if (j.rc === 0) {
        log("烘焙完成：" + j.out, "ok");
        log("验源：ffprobe -show_format " + j.out + " | grep comment", "ok");
      } else {
        log("烘焙失败 rc=" + j.rc + "：" + j.tail, "err");
      }
    })
    .catch(function (e) {
      $("btn-bake").disabled = false; $("job").textContent = "";
      log("烘焙失败：" + e.message, "err");
    });
};
function pollJob(id) {
  return api("/api/job?id=" + id).then(function (j) {
    if (j.running) {
      $("job").textContent = "烘焙中 " + j.elapsed.toFixed(0) + "s";
      return pollJob(id);
    }
    return j;
  });
}
window.addEventListener("resize", fitPreview);
window.addEventListener("message", function (ev) {
  var d = ev.data || {};
  if (typeof d.mcStudioTime === "number") {
    updatePlayhead(d.mcStudioTime, d.total || state.duration);
  }
});
refresh().then(function () {
  if (window.ResizeObserver) {
    new ResizeObserver(function () { fitPreview(); }).observe($("pvwrap"));
  }
}).catch(function (e) { log("状态加载失败：" + e.message, "err"); });
</script>
</body>
</html>
"""



class StudioServer:
    """本地 Studio 服务：预览、重组、TTS、烘焙。仅绑定 127.0.0.1。"""

    def __init__(self, contract_path, spec_path, port, no_sfx=False):
        self.contract_path = os.path.abspath(contract_path)
        self.spec_path = os.path.abspath(spec_path) if spec_path else None
        self.spec_dir = os.path.dirname(self.contract_path) or "."
        self.port = port
        self.no_sfx = no_sfx
        self.job = None
        self.job_seq = 0
        with open(self.contract_path, "r", encoding="utf-8") as f:
            self.current_html = f.read()
        self.current_spec = self._load_spec()

    def _load_spec(self):
        if not self.spec_path or not os.path.isfile(self.spec_path):
            return None
        with open(self.spec_path, "r", encoding="utf-8") as f:
            return json.load(f)

    def _write_spec(self, spec):
        if not self.spec_path:
            raise RuntimeError("未绑定 spec 文件（--serve 时加 --spec <path>）")
        with open(self.spec_path, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False, indent=2)
        self.current_spec = spec

    def _compose(self):
        """当前 spec → 契约 HTML（进程内复用 hf_compose），写回契约文件。"""
        import hf_compose
        import hf_validate
        spec = self.current_spec
        if spec is None:
            raise RuntimeError("未绑定 spec")
        sha = hashlib.sha256(json.dumps(
            spec, sort_keys=True, ensure_ascii=False).encode("utf-8")).hexdigest()
        hf_compose._spec_dir = os.path.dirname(self.spec_path) or "."
        hf_compose._gsap_src = hf_compose._load_gsap()
        html = hf_compose.compose(spec, spec_sha=sha, no_sfx=self.no_sfx)
        findings, errors, warnings, infos = hf_validate.validate(html)
        with open(self.contract_path, "w", encoding="utf-8") as f:
            f.write(html)
        self.current_html = html
        scenes = parse_scenes(html)
        total = parse_total(html, scenes)
        return {
            "ok": errors == 0,
            "errors": errors,
            "duration": total,
            "scenes": scenes,
            "sha": sha[:16],
            "findings": [{"level": lv, "message": msg} for lv, msg in findings],
        }

    def _dims(self, spec):
        import hf_compose
        comp = spec.get("composition", {})
        fmt = comp.get("format")
        if fmt in hf_compose.FORMAT_PRESETS:
            return hf_compose.FORMAT_PRESETS[fmt]
        return (int(comp.get("width", 1080)), int(comp.get("height", 1920)))

    def _start_bake(self):
        if self.job and self.job["proc"].poll() is None:
            raise RuntimeError("已有烘焙任务在跑")
        import hf_compose
        spec = self.current_spec or {}
        w, h = self._dims(spec)
        fps = int(spec.get("composition", {}).get("fps", 30))
        total = parse_total(self.current_html, parse_scenes(self.current_html))
        stem = self.spec_path and os.path.splitext(
            os.path.basename(self.spec_path))[0].replace(".narrated", "") or "out"
        out = os.path.join(os.path.dirname(self.contract_path), stem + ".mp4")
        url = "file://" + urllib.parse.quote(self.contract_path)
        cmd = [
            os.path.join(ROOT_DIR, "build", "bin", "html_bake.app", "Contents",
                         "MacOS", "html_bake"),
            "--url", url, "--out", out,
            "--seconds", f"{total:.2f}", "--fps", str(fps),
            "--width", str(w), "--height", str(h), "--clock", "seek",
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        self.job = {"id": self.job_seq, "proc": proc, "out": out,
                    "start": time.time(), "tail": ""}
        self.job_seq += 1
        return self.job["id"]

    def _job_status(self, job_id):
        if not self.job or self.job["id"] != job_id:
            raise RuntimeError("无此任务")
        proc = self.job["proc"]
        running = proc.poll() is None
        if not running and self.job["tail"] == "":
            out = proc.stdout.read()
            self.job["rc"] = proc.returncode
            self.job["tail"] = out.strip()[-800:]
            m = re.search(r"html_bake: (\S+\.mp4)", self.job["tail"])
            if m:
                self.job["out"] = m.group(1)
        return {"running": running, "rc": self.job.get("rc", -1),
                "out": self.job["out"], "elapsed": time.time() - self.job["start"],
                "tail": self.job["tail"]}


def make_handler(server_ref):
    class Handler(BaseHTTPRequestHandler):
        def _send(self, code, body, ctype="application/json; charset=utf-8"):
            data = body.encode("utf-8") if isinstance(body, str) else body
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(data)

        def _json(self, obj, code=200):
            self._send(code, json.dumps(obj, ensure_ascii=False))

        def log_message(self, *args):
            pass

        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            srv = server_ref
            if path == "/":
                self._send(200, STUDIO_UI, "text/html; charset=utf-8")
            elif path == "/api/state":
                import hf_validate
                findings, errors, _, _ = hf_validate.validate(srv.current_html)
                scenes = parse_scenes(srv.current_html)
                audios = parse_audio_tracks(srv.current_html)
                total = parse_total(srv.current_html, scenes)
                w, h = srv._dims(srv.current_spec or {})
                self._json({"spec": srv.current_spec,
                            "spec_path": srv.spec_path,
                            "duration": total, "scenes": scenes,
                            "audios": audios,
                            "width": w, "height": h,
                            "errors": errors,
                            "findings": [{"level": lv, "message": msg}
                                         for lv, msg in findings]})
            elif path == "/preview":
                self._send(200, inject(srv.current_html),
                           "text/html; charset=utf-8")
            elif path == "/api/job":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                try:
                    self._json(srv._job_status(int(q["id"][0])))
                except (RuntimeError, KeyError, ValueError) as e:
                    self._json({"error": str(e)}, 400)
            else:
                self._serve_asset(path)

        def do_POST(self):
            path = urllib.parse.urlparse(self.path).path
            srv = server_ref
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError as e:
                self._json({"error": f"JSON 解析失败：{e}"}, 400)
                return
            try:
                if path == "/api/recompose":
                    srv.current_spec = body["spec"]
                    srv._write_spec(srv.current_spec)
                    self._json(srv._compose())
                elif path == "/api/tts":
                    srv.current_spec = body["spec"]
                    srv._write_spec(srv.current_spec)
                    proc = subprocess.run(
                        [sys.executable, os.path.join(TOOLS_DIR, "hf_tts.py"),
                         srv.spec_path, "-o", srv.spec_path],
                        capture_output=True, text=True, timeout=600)
                    if proc.returncode != 0:
                        raise RuntimeError(proc.stderr[-400:] or "TTS 失败")
                    srv.current_spec = srv._load_spec()
                    generated = re.search(r"生成 (\d+)", proc.stdout)
                    self._json({"spec": srv.current_spec,
                                "generated": int(generated.group(1)) if
                                generated else 0})
                elif path == "/api/bake":
                    if srv.current_spec is None:
                        raise RuntimeError("未绑定 spec")
                    self._json({"job": srv._start_bake()})
                else:
                    self._json({"error": "unknown endpoint"}, 404)
            except (RuntimeError, ValueError, OSError, KeyError) as e:
                self._json({"error": str(e)}, 400)

        def _serve_asset(self, path):
            srv = server_ref
            rel = urllib.parse.unquote(path.lstrip("/"))
            ext = os.path.splitext(rel)[1].lower()
            if ext not in FILE_EXTS:
                self._send(404, "not found", "text/plain")
                return
            base = os.path.realpath(srv.spec_dir)
            full = os.path.realpath(os.path.join(base, rel))
            if not (full == base or full.startswith(base + os.sep)):
                self._send(404, "not found", "text/plain")
                return
            if not os.path.isfile(full):
                self._send(404, "not found", "text/plain")
                return
            ctype = ("audio/wav" if ext == ".wav" else
                     "audio/mpeg" if ext == ".mp3" else
                     "audio/mp4" if ext in (".m4a", ".aac") else
                     "text/html; charset=utf-8" if ext == ".html" else
                     "application/json" if ext == ".json" else
                     "text/plain; charset=utf-8")
            with open(full, "rb") as f:
                self._send(200, f.read(), ctype)

    return Handler


def serve(contract_path, spec_path, port, no_sfx):
    contract_path = os.path.abspath(contract_path)
    spec_dir = os.path.dirname(contract_path) or "."
    if spec_path is None:
        # 契约页生成器注释携带 spec 文件名（hf_compose 写入）——自动发现
        m = re.search(r'spec:([^|]+)\|', _read_head(contract_path))
        if m:
            candidate = os.path.join(os.path.dirname(contract_path),
                                     m.group(1).strip())
            if os.path.isfile(candidate):
                spec_path = candidate
    srv = StudioServer(contract_path, spec_path, port, no_sfx)

    class PassThrough(ThreadingHTTPServer):
        daemon_threads = True

    httpd = PassThrough(("127.0.0.1", port), make_handler(srv))
    port = httpd.server_address[1]
    print(f"xmedia Studio: http://127.0.0.1:{port}  "
          f"(contract: {os.path.basename(contract_path)}, "
          f"spec: {srv.spec_path or '未绑定——只读预览'})  Ctrl+C 退出")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nxmedia Studio: 已退出")
    return 0


def _read_head(path, limit=4096):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read(limit)


def main():
    parser = argparse.ArgumentParser(
        description="xmedia Studio：契约 HTML 预览注入 / 本地 Studio 服务")
    parser.add_argument("html", help="契约 HTML（hf_compose 产出或过校验的手写页）")
    parser.add_argument("-o", "--output", default=None,
                        help="注入模式：输出 studio HTML（默认 <stem>.studio.html）")
    parser.add_argument("--serve", action="store_true",
                        help="启动本地 Studio 服务（浏览器 UI：编辑 spec → "
                             "热重组 → TTS → 烘焙）")
    parser.add_argument("--spec", default=None,
                        help="服务模式：绑定的 spec JSON 路径（缺省从契约页"
                             "生成器注释自动发现；无则只读预览）")
    parser.add_argument("--port", type=int, default=8120)
    parser.add_argument("--no-sfx", action="store_true",
                        help="服务模式：重组时关闭 SFX")
    args = parser.parse_args()

    if args.serve:
        sys.exit(serve(args.html, args.spec, args.port, args.no_sfx))

    src = os.path.abspath(args.html)
    with open(src, "r", encoding="utf-8") as f:
        html = f.read()
    already_studio = MARK_BEGIN in html
    if args.output:
        out = os.path.abspath(args.output)
    elif already_studio:
        out = src  # 对 studio 页重复运行 = 原地更新，不生成套娃文件名
    else:
        stem, ext = os.path.splitext(src)
        out = f"{stem}.studio{ext or '.html'}"

    scenes = parse_scenes(html)
    if not scenes:
        print("hf_studio: 页面无 data-start/data-duration 场景——"
              "不是契约 HTML？先跑 hf_compose.py 或 hf_validate.py",
              file=sys.stderr)
        sys.exit(1)

    with open(out, "w", encoding="utf-8") as f:
        f.write(inject(html))
    print(f"hf_studio: {out} — {len(scenes)} 场景 / "
          f"{parse_total(html, scenes):.1f}s，浏览器打开即预览")


if __name__ == "__main__":
    main()
