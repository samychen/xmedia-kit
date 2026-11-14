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
  [data-start] {{ cursor: pointer; }}
  .mc-studio-sel {{ outline: 3px solid #ff5c7a; outline-offset: -3px; }}
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
  var FPS = (function () {{
    var root = document.querySelector('[data-composition-id]');
    var f = root ? parseFloat(root.getAttribute('data-fps')) : NaN;
    return isFinite(f) && f > 0 ? f : 30;
  }})();
  var TOTAL_FR = Math.round(TOTAL * FPS);
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
    label.textContent = sec.toFixed(1) + "s · 帧 " +
      Math.min(TOTAL_FR, Math.round(sec * FPS)) + "/" + TOTAL_FR;
    // Studio 服务模式：把播放时间回报给父页（多轨时间轴播放头联动）。
    if (window.parent !== window) {{
      try {{ window.parent.postMessage({{ mcStudioTime: sec, total: TOTAL,
        frame: Math.round(sec * FPS), frames: TOTAL_FR }},
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
    if (typeof d.mcStudioHighlight === "number") {{
      var sc = SCENES[d.mcStudioHighlight];
      var el = sc ? document.getElementById(sc.id) : null;
      if (el) {{
        el.classList.add("mc-studio-sel");
        if (lastSel && lastSel !== el) {{
          lastSel.classList.remove("mc-studio-sel");
        }}
        lastSel = el;
      }}
    }}
  }});
  var lastSel = null;
  // 预览点选联动：点击场景元素 → 高亮 + 上报父页选中（Remotion 式
  // "预览里点 → 编辑器选中"闭环）。
  document.addEventListener("click", function (ev) {{
    var el = ev.target.closest("[data-start]");
    if (!el || el.tagName === "AUDIO") {{ return; }}
    var idx = -1;
    for (var i = 0; i < SCENES.length; i++) {{
      if (SCENES[i].id === el.id) {{ idx = i; break; }}
    }}
    if (idx < 0) {{ return; }}
    el.classList.add("mc-studio-sel");
    if (lastSel && lastSel !== el) {{
      lastSel.classList.remove("mc-studio-sel");
    }}
    lastSel = el;
    if (window.parent !== window) {{
      try {{ window.parent.postMessage({{ mcStudioSelect: idx }}, "*");
      }} catch (e) {{}}
    }}
  }}, true);
  document.addEventListener("keydown", function (ev) {{
    if (ev.code === "Space" && ev.target === document.body) {{
      ev.preventDefault(); playing ? pause() : play();
    }} else if (ev.code === "ArrowLeft" || ev.code === "ArrowRight") {{
      // 帧步进：±1 帧（Shift = ±10 帧）——Remotion 式逐帧编辑。
      ev.preventDefault(); pause();
      var step = (ev.code === "ArrowRight" ? 1 : -1) /
                 FPS * (ev.shiftKey ? 10 : 1);
      t = Math.max(0, Math.min(TOTAL, t + step));
      drive(t);
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

# Studio UI：独立文件按请求读取——UI 改动刷新浏览器即生效
# （无需重启服务，工程级开发闭环）。
STUDIO_UI_PATH = os.path.join(TOOLS_DIR, "studio_ui.html")


def studio_ui():
    try:
        with open(STUDIO_UI_PATH, "r", encoding="utf-8") as f:
            return f.read()
    except OSError:
        return "<html><body><p>studio_ui.html 缺失</p></body></html>"



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
                self._send(200, studio_ui(), "text/html; charset=utf-8")
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
