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
import copy
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TOOLS_DIR)
MARK_BEGIN = "<!-- xmedia-kit studio:begin -->"
MARK_END = "<!-- xmedia-kit studio:end -->"
AUDIO_EXTS = {".wav", ".mp3", ".m4a", ".aac"}
# hf_import 支持的实拍素材类型：导入 ~/Downloads/*.mkv 之类的素材后，
# 预览页也要能经 HTTP 取到（否则 <video src="/Users/..."> 直接 404）。
IMPORT_MEDIA_EXTS = ({".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v"} |
                     {".flac", ".ogg"} | {".srt", ".vtt"})
FILE_EXTS = AUDIO_EXTS | IMPORT_MEDIA_EXTS | {
    ".txt", ".json", ".html", ".png", ".jpg", ".jpeg", ".webp", ".svg",
    ".js", ".css"}
# 契约页里 hf_compose 写入的绝对文件系统路径（assets/sfx/*.wav）在
# file:// 烘焙下可用，但 HTTP 预览会把 "/Users/..." 当成 URL 绝对路径而
# 404。服务模式下改写为这个只读映射前缀，契约文件本身保持不变。
ROOT_ASSET_PREFIX = "/__xmedia_root__/"
# 导入的外部素材（不在项目根内）绝对路径映射。只允许 spec 显式登记过的
# 文件，避免本地服务把整块磁盘暴露给预览页。
EXTERNAL_ASSET_PREFIX = "/__xmedia_media__/"
# 视频缩略图（从源视频抽帧）缓存目录与只读服务前缀。仅允许 spec 登记过
# 或项目 / spec 目录内的视频，绝不暴露任意磁盘路径。
THUMBS_PREFIX = "/__xmedia_thumbs__/"
THUMB_EXTS = {".jpg": "image/jpeg", ".png": "image/png"}
# 浏览器上传走内存，超大素材请用「本机目录路径」零拷贝导入。
MAX_UPLOAD_BYTES = 512 * 1024 * 1024

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


PREVIEW_MIX_URL = "/__xmedia_preview_mix__.wav"


def _track_local_path(src, contract_path):
    """契约页音轨 src → 本地文件绝对路径（含 ROOT_ASSET_PREFIX 映射）。"""
    p = urllib.parse.unquote(src)
    if p.startswith(EXTERNAL_ASSET_PREFIX):
        p = "/" + p[len(EXTERNAL_ASSET_PREFIX):]
    elif p.startswith(ROOT_ASSET_PREFIX):
        p = os.path.join(ROOT_DIR, p[len(ROOT_ASSET_PREFIX):])
    elif not os.path.isabs(p):
        p = os.path.join(os.path.dirname(contract_path), p)
    return os.path.realpath(p)


def build_preview_mix(contract_path, html, total_s):
    """把契约页的多条 <audio> 预混成单条 WAV（仅服务模式预览使用）。

    播放中途不再有任何 HTMLAudioElement 首次 play()：Safari 的音轨解码器 /
    输出设备初始化是同步的，会阻塞与父页共享的主线程，表现为第二个、第三个
    音效起点前游标突然停住数帧。混音在服务端按 data-start / data-volume
    做一次，契约页、注入模式的单文件、file:// 烘焙链路全部不变。
    """
    tracks = parse_audio_tracks(html)
    pairs = []
    for t in tracks:
        p = _track_local_path(t["src"], contract_path)
        if os.path.isfile(p):
            pairs.append((p, t))
    if not pairs or total_s <= 0:
        return None
    cmd = ["ffmpeg", "-v", "error", "-y"]
    filt = []
    for i, (p, t) in enumerate(pairs):
        cmd += ["-i", p]
        delay = max(0, int(round(t["start"] * 1000)))
        vol = max(0.0, min(4.0, float(t["vol"])))
        filt.append(
            f"[{i}:a]aformat=sample_rates=48000:channel_layouts=stereo,"
            f"adelay={delay}:all=1,volume={vol:.4f}[a{i}]")
    ins = "".join(f"[a{i}]" for i in range(len(pairs)))
    filt.append(
        f"{ins}amix=inputs={len(pairs)}:duration=longest:normalize=0,"
        f"apad,atrim=0:{total_s:.3f}[out]")
    cmd += ["-filter_complex", ";".join(filt), "-map", "[out]",
            "-ac", "2", "-ar", "48000", "-c:a", "pcm_s16le"]
    fd, tmp = tempfile.mkstemp(prefix="xmedia_preview_", suffix=".wav")
    os.close(fd)
    try:
        cmd += ["-f", "wav", tmp]
        proc = subprocess.run(cmd, capture_output=True, timeout=60)
        if proc.returncode != 0 or os.path.getsize(tmp) < 1024:
            return None
        with open(tmp, "rb") as f:
            return f.read()
    except (OSError, subprocess.SubprocessError):
        return None  # ffmpeg 缺失 / 素材异常 → 自动降级到多轨预览引擎
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass


def build_chrome(total_s, scenes, mix_url=""):
    """注入块：注释标记包裹的 chrome DOM + 驱动脚本。

    mix_url 非空（HTTP 预览且混音可用）时改用单条预混音音轨，播放中途
    不再调用任何音轨的 play()；为空则保留契约页的多 <audio> 引擎。

    驱动同时暴露 window.__mcStudioAPI（同源父页直调）与 postMessage 协议
    （mcStudioCommand / mcStudioSeek / mcStudioHighlight），并上报
    mcStudioState（时间、帧号、播放/循环/速率）。`?embed=1` 且处于 iframe
    时隐藏内嵌 chrome、把快捷键转发给父页，由 Studio 服务 UI 统一接管传输条。
    注入模式（单文件浏览器打开）仍使用自带悬浮传输条。
    """
    scenes_json = json.dumps(scenes, ensure_ascii=False)
    mix_literal = json.dumps(mix_url or "")
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
  /* Studio 实时预览是真实浏览器，没有 CEF 烘焙侧的原生视频层，
     必须让 .scene-video 可见；烘焙侧不注入本 chrome，不受影响。 */
  html.mc-studio .scene-video {{ visibility: visible; }}
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
  // 标记当前为 Studio 实时预览（区别于 CEF 烘焙）：让 .scene-video 可见、
  // 由下面的播放逻辑驱动视频。烘焙侧不注入本脚本，视频保持 visibility:hidden
  // 由 CEF 原生视频层供给画面。
  document.documentElement.classList.add("mc-studio");
  var TOTAL = {total_s};
  var MIX_URL = {mix_literal};   // 单条预混音预览音轨（空 = 多轨引擎）
  var SCENES = {scenes_json};
  var FPS = (function () {{
    var root = document.querySelector('[data-composition-id]');
    var f = root ? parseFloat(root.getAttribute('data-fps')) : NaN;
    return isFinite(f) && f > 0 ? f : 30;
  }})();
  var TOTAL_FR = Math.round(TOTAL * FPS);
  var playing = false, t = 0, raf = 0, last = 0;
  var loop = false, rate = 1;
  // 嵌入 Studio 服务模式：父页自带传输条，内嵌 chrome 让位。
  var EMBED = /[?&]embed=1(?:&|$)/.test(location.search) &&
              window.parent !== window;
  var bar = document.getElementById("__mc-studio-bar");
  var btn = document.getElementById("__mc-studio-play");
  var label = document.getElementById("__mc-studio-time");

  /* ---- 预览音频引擎 --------------------------------------------------
     契约页的 <audio data-start/data-duration/data-volume> 是烘焙侧的
     确定性混音清单，本身不带 autoplay/controls——浏览器里默认完全静音。
     这里把它们当作多轨时间轴素材：播放头进入窗口即按对应偏移起播，
     离开或暂停即停；只在「显式 seek」与「起播」时定位一次。

     关键：播放中绝不逐帧 seek。每次 currentTime 赋值都会让媒体重新
     缓冲、短暂静音，用 rAF 累加的时间去校正音频时钟会形成「落后→
     seek→再缓冲→再落后」的循环，听感就是断断续续。

     播放头由严格单调的 rAF 时钟驱动，正常播放时音频时钟不参与步长：
     直接把 t 覆盖成 audioClock() 会在音轨起播/切换时把游标钉在素材
     起点（新轨缓冲中 currentTime=0）；而把音频时钟当作「这一帧目标」
     做纠偏，则会在音轨起播滞后时连续多帧压低步长，游标明显减速。
     只有偏差超过死区才以极小幅度缓慢收敛。
     只注入预览页，不改写契约文件，烘焙链路不受影响。 */
  // 音画时钟纠偏：正常播放绝不干预步长；只有明显失步才缓慢收敛。
  var SKEW_DEADBAND = 0.18, SKEW_PULL = 0.002;
  var audioPend = false;
  var AUDIOS = [];
  var audioOn = true, masterVol = 1, audioBlocked = false, audioActive = 0;
  (function initAudio() {{
    if (MIX_URL) {{
      // 单条预混音：所有素材已在服务端按 start/volume 对齐成一条时间轴。
      // 播放中途不再有任何元素首次 play()，Safari 的解码器/输出设备同步
      // 初始化开销全部集中在起播那一帧，不再打断 00:01.37 / 00:03.90
      // 附近的渲染帧。契约页原有的 <audio> 保留给烘焙链路，但不加载、不播放。
      [].forEach.call(document.querySelectorAll("audio"), function (el) {{
        el.preload = "none";
        el.setAttribute("data-mc-audio", "mixed");
      }});
      var mix = document.createElement("audio");
      mix.src = MIX_URL;
      mix.preload = "auto";
      mix.volume = masterVol;
      mix.muted = false;
      mix.playbackRate = 1;
      mix.setAttribute("data-mc-audio", "idle");
      document.body.appendChild(mix);
      AUDIOS.push({{ el: mix, start: 0, dur: TOTAL + 0.5, vol: 1,
                     pending: false, warming: false, warmed: false }});
      return;
    }}
    [].forEach.call(document.querySelectorAll("audio"), function (el) {{
      var st = parseFloat(el.getAttribute("data-start") || "0");
      var du = parseFloat(el.getAttribute("data-duration") || "0");
      var vo = parseFloat(el.getAttribute("data-volume") || "1");
      st = isFinite(st) ? st : 0;
      du = (isFinite(du) && du > 0) ? du : Infinity;
      vo = isFinite(vo) ? vo : 1;
      el.preload = "auto";
      el.volume = Math.max(0, Math.min(1, vo * masterVol));
      el.muted = false;
      el.playbackRate = 1;
      el.setAttribute("data-mc-audio", "idle");
      AUDIOS.push({{ el: el, start: st, dur: du, vol: vo,
                     pending: false, warming: false, warmed: false }});
    }});
  }})();
  // 预热：页面加载后把每条音轨静音起播一次再停回 0，让解码器与音频
  // 输出管线提前初始化。否则播放中途第 2、3 个 SFX（以及各段旁白）首次
  // play() 会同步初始化设备 + 解码 WAV，阻塞主线程数帧，垂直游标就会在
  // 00:01.37 / 00:03.90 前几帧突然变卡。静音自动播放无需用户手势；
  // 被策略拦截时不置 warmed，等首次手势 unlockAudio() 重试。
  function warmAudio() {{
    AUDIOS.forEach(function (a) {{
      if (a.warmed || a.warming) {{ return; }}
      var el = a.el;
      a.warming = true;
      try {{
        el.muted = true;
        var p = el.play();
        var ok = function () {{
          try {{ el.pause(); el.currentTime = 0; }} catch (e) {{}}
          el.muted = !audioOn;
          a.warming = false; a.warmed = true;
          setAudioAttr(a, "idle");
        }};
        var fail = function () {{
          try {{ el.pause(); el.currentTime = 0; }} catch (e) {{}}
          el.muted = !audioOn;
          a.warming = false;
        }};
        if (p && p.then) {{ p.then(ok, fail); }} else {{ ok(); }}
        // 兜底：万一 play() 的 Promise 既不 resolve 也不 reject，别让
        // a.warming 永远为 true 而把这条音轨从 syncAudio 里排除掉。
        setTimeout(function () {{ if (a.warming) {{ fail(); }} }}, 800);
      }} catch (e) {{
        el.muted = !audioOn;
        a.warming = false;
      }}
    }});
  }}
  warmAudio();
  // 音频参考时钟：取「与当前播放时间最接近」且确实在推进的音轨，换算回
  // 全局时间轴（秒）。无可用轨（静音 / 纯画面段 / 素材间隙 / 起播缓冲）
  // 时返回 -1，此时仍由 rAF 计时。
  //  1) 必须 currentTime > 0.02：刚起播或还在缓冲的轨 currentTime≈0，
  //     早先用 a.start + 0 当主时钟，会把 t 钉在素材起点整整若干帧；
  //  2) 不按 DOM 顺序取「第一条」，而是取最接近 ref 的一条：音轨陆续
  //     启停换源时不会带来时间跳变。
  function audioClock(ref) {{
    var best = -1, bestErr = Infinity;
    for (var i = 0; i < AUDIOS.length; i++) {{
      var a = AUDIOS[i], el = a.el;
      if (a.warming || el.paused || el.ended || el.readyState < 2 ||
          el.currentTime <= 0.02) {{ continue; }}
      var c = a.start + el.currentTime;
      var err = Math.abs(c - ref);
      if (err < bestErr) {{ bestErr = err; best = c; }}
    }}
    return best;
  }}
  function audioState() {{
    return {{ enabled: audioOn, blocked: audioBlocked, volume: masterVol,
      tracks: AUDIOS.length, active: audioActive }};
  }}
  function setAudioAttr(a, s) {{
    if (a.el.getAttribute("data-mc-audio") !== s) {{
      a.el.setAttribute("data-mc-audio", s);
    }}
  }}
  function applyVolumes() {{
    AUDIOS.forEach(function (a) {{
      a.el.muted = !audioOn;
      a.el.volume = Math.max(0, Math.min(1, a.vol * masterVol));
    }});
  }}
  function syncAudio(sec, isPlaying, forceSeek) {{
    var active = 0;
    var pr = Math.max(0.1, Math.min(4, rate));
    AUDIOS.forEach(function (a) {{
      if (a.warming) {{ return; }}  // 预热中不碰，避免状态互相打架
      var el = a.el, local = sec - a.start;
      var inside = local >= -0.02 && local < a.dur;
      // 每帧重写同一个 playbackRate 也可能触发媒体管线重配，只在变化时写。
      if (el.playbackRate !== pr) {{ el.playbackRate = pr; }}
      if (!audioOn || !isPlaying || !inside) {{
        if (!el.paused) {{ el.pause(); }}
        // 暂停 / 静音 / 窗口外时只在显式 seek 对位，避免暂停态反复缓冲。
        if (inside && forceSeek) {{
          try {{ el.currentTime = Math.max(0, local); }} catch (e) {{}}
        }}
        setAudioAttr(a, audioOn ? "idle" : "muted");
        return;
      }}
      // 短音效播完后，窗口尚未结束的这段时间别再每帧定位：给 paused
      // 媒体反复赋 currentTime 会重新解码/缓冲（边界掉帧），还可能让
      // 同一音效被反复 play()。显式 seek 时走正常路径重新起播。
      if (!forceSeek && el.paused &&
          el.getAttribute("data-mc-audio") === "done") {{ return; }}
      var want = Math.max(0, local);
      // 只在「显式 seek」或「起播」时定位一次。播放中绝不逐帧 seek：
      // 每次 currentTime 赋值都会重新缓冲、短暂静音，用 rAF 时间反复
      // 校正音频时钟会形成「落后→seek→再缓冲→再落后」的循环。
      if (forceSeek || el.paused) {{
        try {{ el.currentTime = want; }} catch (e) {{}}
      }}
      // 素材实长可能短于 data-duration（SFX 常见）：播完后停手，避免
      // 同一音效在窗口内被每帧重新 play() 反复重播。
      if (!forceSeek && (el.ended ||
          (isFinite(el.duration) && el.duration > 0 &&
           el.currentTime >= el.duration - 0.03))) {{
        if (!el.paused) {{ el.pause(); }}
        setAudioAttr(a, "done");
        return;
      }}
      // 自动播放被拦后停手，等真实用户手势 unlockAudio() 再重试，
      // 避免每帧一次被拒的 play() / 状态风暴。
      if (el.paused && !a.pending && !audioBlocked) {{
        a.pending = true;
        var p = el.play();
        if (p && p.then) {{
          p.then(function () {{ a.pending = false; }},
                 function () {{
                   a.pending = false;
                   audioBlocked = true;
                   setAudioAttr(a, "blocked");
                   report();
                 }});
        }} else {{ a.pending = false; }}
      }}
      if (!el.paused) {{ active++; setAudioAttr(a, "playing"); }}
    }});
    audioActive = active;
  }}
  // 浏览器自动播放策略：首次真实用户手势到达后重试被拦的 play()。
  function unlockAudio() {{
    audioBlocked = false;
    applyVolumes();
    warmAudio();  // 手势后重试被自动播放策略拦下的预热
    if (audioOn && playing) {{ syncAudio(t, true, false); }}
    report();
  }}
  document.addEventListener("pointerdown", unlockAudio, true);
  document.addEventListener("keydown", unlockAudio, true);

  function drive(sec, forceSeek) {{
    // 状态上报放在最前：父页播放头先拿到本帧时间，后面的 GSAP 定位、
    // 场景可见性切换与音轨首次解码都不会推迟这条消息。
    report();
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
    // 视频元素同步到时间轴位置（Studio 实时预览是真实浏览器，需显式
    // play/pause；烘焙侧由 CEF 原生视频层供给画面，不依赖此逻辑）。
    // 播放态只在进入本窗口时 play 一次、离开时 pause——绝不逐帧 seek，
    // 否则每次 currentTime 赋值都会让解码器重新缓冲（同音频的卡顿根因）。
    try {{
      var vids = document.querySelectorAll("video");
      for (var vi = 0; vi < vids.length; vi++) {{
        var v = vids[vi];
        var vs = parseFloat(v.getAttribute("data-start") || "0");
        var vd = parseFloat(v.getAttribute("data-duration") || "0");
        var off = sec - vs;
        var inWin = off >= -0.02 && off <= vd + 0.05;
        if (playing && inWin) {{
          if (v.paused) {{
            if (Math.abs(v.currentTime - Math.max(0, off)) > 0.05) {{
              try {{ v.currentTime = Math.max(0, off); }} catch (e) {{}}
            }}
            var vp = v.play();
            if (vp && vp.catch) {{ vp.catch(function () {{}}); }}
          }}
        }} else {{
          if (!v.paused) {{ try {{ v.pause(); }} catch (e) {{}} }}
          if (inWin) {{  // 暂停态拖拽：定位到对应帧以便预览
            if (Math.abs(v.currentTime - Math.max(0, off)) > 0.05) {{
              try {{ v.currentTime = Math.max(0, off); }} catch (e) {{}}
            }}
          }} else if (v.currentTime !== 0) {{
            try {{ v.currentTime = 0; }} catch (e) {{}}  // 离开窗口复位，下次从头播
          }}
        }}
      }}
    }} catch (e) {{}}
    // 音频同步移出 rAF 帧：新音轨首次 play() 会同步初始化解码器与输出
    // 设备。显式 seek 仍同步对位（拖拽 / 暂停态无流畅度要求）。
    if (forceSeek) {{
      syncAudio(sec, playing, true);
    }} else if (!audioPend) {{
      audioPend = true;
      setTimeout(function () {{
        audioPend = false;
        syncAudio(t, playing, false);
      }}, 0);
    }}
    bar.value = TOTAL > 0 ? Math.round(sec / TOTAL * 1000) : 0;
    label.textContent = sec.toFixed(1) + "s · 帧 " +
      Math.min(TOTAL_FR, Math.round(sec * FPS)) + "/" + TOTAL_FR;
  }}
  function report() {{
    if (window.parent === window) {{ return; }}
    try {{ window.parent.postMessage({{
      mcStudioTime: t, total: TOTAL,
      frame: Math.round(t * FPS), frames: TOTAL_FR, fps: FPS,
      playing: playing, loop: loop, rate: rate, audio: audioState(),
      mcStudioState: {{ time: t, frame: Math.round(t * FPS),
        frames: TOTAL_FR, fps: FPS, total: TOTAL,
        playing: playing, loop: loop, rate: rate, audio: audioState() }}
    }}, "*"); }} catch (e) {{}}
  }}
  // 播放时钟：t 严格单调，只由 rAF 累加推进，正常播放时完全匀速。
  // 两种曾导致边界卡顿的做法都不再用：
  //  1) 直接 t = clock：新轨缓冲中 currentTime=0，把游标钉在 a.start；
  //  2) 把 clock 当作这一帧的目标做牵引：新 SFX 起播时音频时钟会短暂
  //     滞后，want 恒为负并被压到步长下限，连续多帧只有 60% 速度，
  //     观感就是「第二/第三个音效开始时游标顿一下，之后正常」。
  // 现在只有偏差超过 SKEW_DEADBAND 才以 SKEW_PULL/帧 缓慢收敛，
  // 日常播放（含所有音轨边界）步长恒为 dt*rate。
  function frame(now) {{
    if (!playing) {{ return; }}
    var dt = (now - last) / 1000; last = now;
    if (!(dt > 0)) {{ dt = 0; }}
    dt = Math.min(dt, 0.1);  // 切后台 / 掉帧时不让时间一次跳太多
    var step = dt * rate;
    var clock = audioClock(t), skew = clock - t;
    if (clock >= 0 && Math.abs(skew) > SKEW_DEADBAND &&
        Math.abs(skew) < 0.6) {{
      var pull = Math.max(-SKEW_PULL, Math.min(SKEW_PULL, skew * 0.01));
      step = Math.max(step * 0.75, step + pull);  // 单调下限，绝不倒退
    }}
    t = Math.min(TOTAL, t + step);
    drive(t);
    if (t >= TOTAL) {{
      if (loop || window.__mcStudioLoop) {{
        // 回到起点时重新定位音轨：避免上一轮 done 短路让音效不再起播。
        playing = true; last = now; goto(0);
        raf = requestAnimationFrame(frame);
        return;
      }}
      playing = false; btn.textContent = "▶"; report();
      return;
    }}
    raf = requestAnimationFrame(frame);
  }}
  function play() {{
    if (playing) {{ return; }}
    if (t >= TOTAL) {{ t = 0; }}
    playing = true; last = performance.now();
    btn.textContent = "⏸";
    drive(t, true);  // 立刻起播音轨，不等下一帧
    raf = requestAnimationFrame(frame);
  }}
  // 暂停时显式停掉正在播放的视频：drive() 里的 play/pause 逻辑只在播放态
  // 或 seek 时运行，rAF 一停视频会自己继续播（用户看到「暂停没生效」）。
  function pauseVideos() {{
    try {{
      var vids = document.querySelectorAll("video");
      for (var i = 0; i < vids.length; i++) {{
        var v = vids[i];
        if (!v.paused) {{ try {{ v.pause(); }} catch (e) {{}} }}
      }}
    }} catch (e) {{}}
  }}
  function pause() {{
    playing = false; btn.textContent = "▶";
    if (raf) {{ cancelAnimationFrame(raf); raf = 0; }}
    pauseVideos();
    syncAudio(t, false, false);
    report();
  }}
  function goto(sec) {{
    t = Math.max(0, Math.min(TOTAL, sec));
    drive(t, true);
  }}
  function step(frames) {{
    pause(); goto(t + frames / FPS);
  }}
  function setLoop(on) {{ loop = !!on; window.__mcStudioLoop = loop; report(); }}
  function setRate(r) {{
    rate = Math.max(0.1, Math.min(4, r || 1));
    syncAudio(t, playing, false);
    report();
  }}
  function setMuted(m) {{
    audioOn = !m;
    applyVolumes();
    syncAudio(t, playing, false);
    report();
  }}
  function setVolume(v) {{
    var n = Number(v);
    masterVol = isFinite(n) ? Math.max(0, Math.min(1, n)) : 1;
    applyVolumes();
    report();
  }}
  // 同源父页可直接调用的预览 API（Studio 服务模式）；postMessage 为降级路径。
  window.__mcStudioAPI = {{
    play: play, pause: pause,
    toggle: function () {{ playing ? pause() : play(); }},
    seek: goto, step: step, setLoop: setLoop, setRate: setRate,
    setMuted: setMuted, setVolume: setVolume,
    enableAudio: unlockAudio, audioState: audioState,
    state: function () {{
      return {{ time: t, total: TOTAL, frame: Math.round(t * FPS),
        frames: TOTAL_FR, fps: FPS, playing: playing, loop: loop,
        rate: rate, audio: audioState() }};
    }}
  }};
  btn.addEventListener("click", function () {{
    playing ? pause() : play();
  }});
  bar.addEventListener("input", function () {{
    pause(); goto(TOTAL * (parseFloat(bar.value) / 1000));
  }});
  document.getElementById("__mc-studio-scenes")
    .addEventListener("click", function (ev) {{
      var g = ev.target.getAttribute && ev.target.getAttribute("data-goto");
      if (g === null || g === undefined) {{ return; }}
      pause(); goto(parseFloat(g));
    }});
  // Studio 服务的时间轴联动：父页面 postMessage {{mcStudioSeek: 秒}}。
  window.addEventListener("message", function (ev) {{
    var d = ev.data || {{}};
    if (typeof d.mcStudioSeek === "number") {{
      pause(); goto(d.mcStudioSeek);
    }}
    if (d.mcStudioCommand) {{
      var c = d.mcStudioCommand, v = d.mcStudioValue;
      if (c === "play") {{ play(); }}
      else if (c === "pause") {{ pause(); }}
      else if (c === "toggle") {{ playing ? pause() : play(); }}
      else if (c === "step") {{ step(Number(v) || 1); }}
      else if (c === "goto") {{ pause(); goto(Number(v) || 0); }}
      else if (c === "loop") {{ setLoop(v); }}
      else if (c === "rate") {{ setRate(Number(v) || 1); }}
      else if (c === "setMuted") {{ setMuted(!!v); }}
      else if (c === "setVolume") {{ setVolume(v); }}
      else if (c === "enableAudio") {{ unlockAudio(); }}
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
    // 嵌入模式：快捷键统一交给父页预览区处理，避免两套逻辑抢焦点。
    if (EMBED) {{
      if (ev.code === "Space" || ev.code.indexOf("Arrow") === 0) {{
        ev.preventDefault();
      }}
      if (window.parent !== window) {{
        try {{ window.parent.postMessage({{
          mcStudioKey: ev.code, shiftKey: ev.shiftKey,
          metaKey: ev.metaKey, ctrlKey: ev.ctrlKey
        }}, "*"); }} catch (e) {{}}
      }}
      return;
    }}
    if (ev.code === "Space" && ev.target === document.body) {{
      ev.preventDefault(); playing ? pause() : play();
    }} else if (ev.code === "ArrowLeft" || ev.code === "ArrowRight") {{
      // 帧步进：±1 帧（Shift = ±10 帧）——Remotion 式逐帧编辑。
      ev.preventDefault();
      var frames = (ev.code === "ArrowRight" ? 1 : -1) *
                   (ev.shiftKey ? 10 : 1);
      step(frames);
    }}
  }});
  // 嵌入模式：内嵌传输条让位给父页预览区的统一控制条。
  if (EMBED) {{
    var chrome = document.getElementById("__mc-studio-chrome");
    if (chrome) {{ chrome.style.display = "none"; }}
  }}
  drive(0);
}})();
</script>
{MARK_END}'''


def rewrite_asset_srcs(html, external_files=None):
    """HTTP 预览专用：把绝对文件路径映射为可服务 URL。

    hf_compose 为内置 SFX 写入的是绝对路径（供 file:// 烘焙解析），浏览器
    在 http://127.0.0.1:8120/preview 下会请求 `/Users/.../whoosh.wav`。
    项目根内的文件走 ROOT_ASSET_PREFIX；从外部目录导入的素材只有在
    external_files（spec 显式登记过的 realpath 集合）里才映射到
    EXTERNAL_ASSET_PREFIX。spec 目录下的相对路径、外部 URL、data: 一律
    不动。契约文件内容不变，烘焙语义不受影响。
    """
    base = os.path.realpath(ROOT_DIR)
    external = set(external_files or ())

    def repl(m):
        src = m.group(2)
        if not src.startswith("/") or src.startswith("//"):
            return m.group(0)
        full = os.path.realpath(urllib.parse.unquote(src))
        # 已登记在白名单的外部素材：无论文件此刻是否存在都改写为只读 URL，
        # 避免文件被移动/删除后退化为裸绝对路径（浏览器会当成 URL → 404）。
        if full in external:
            return (m.group(1) + EXTERNAL_ASSET_PREFIX +
                    urllib.parse.quote(full.lstrip("/"), safe="/") + m.group(3))
        if not os.path.isfile(full):
            return m.group(0)
        if full.startswith(base + os.sep):
            rel = os.path.relpath(full, base).replace(os.sep, "/")
            return (m.group(1) + ROOT_ASSET_PREFIX +
                    urllib.parse.quote(rel) + m.group(3))
        return m.group(0)

    return re.sub(r'(<(?:audio|video|source|img)\b[^>]*?\bsrc=")([^"]+)(")',
                  repl, html, flags=re.S)


def inject(html, for_http=False, mix_url="", external_files=None):
    """幂等注入：已存在 chrome 则先剥离旧块再重注入（spec 变更后可重生成）。

    for_http=True 时额外改写契约页的绝对素材路径（仅服务模式需要）。
    mix_url 非空时预览改用单条预混音（仅服务模式，契约文件不变）。
    """
    if MARK_BEGIN in html:
        html = re.sub(re.escape(MARK_BEGIN) + r"[\s\S]*?" + re.escape(MARK_END),
                      "", html)
    if for_http:
        html = rewrite_asset_srcs(html, external_files)
    scenes = parse_scenes(html)
    total = parse_total(html, scenes)
    chrome = build_chrome(total, scenes, mix_url)
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
        self._mix_key = None
        self._mix_bytes = None
        # spec 引用的外部素材（导入 / Agent 写入）→ HTTP 预览只读白名单。
        self.external_files = set()
        self.last_import_dir = ""
        # 导入前的 spec 快照（仅留最近一次），供「撤销导入」回滚。
        self._undo_spec = None
        self._undo_label = ""
        if self.current_spec:
            self._register_spec_paths(self.current_spec)

    def preview_mix(self):
        """按当前契约页内容缓存单条预览混音；重组后自动失效重算。"""
        key = hashlib.sha1(self.current_html.encode("utf-8")).hexdigest()
        if key == self._mix_key:
            return self._mix_bytes
        scenes = parse_scenes(self.current_html)
        total = parse_total(self.current_html, scenes)
        self._mix_bytes = build_preview_mix(
            self.contract_path, self.current_html, total)
        self._mix_key = key
        return self._mix_bytes

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
        # 所有写回 spec 的路径（重组 / Agent / 导入 / TTS）统一登记素材，
        # 预览页才能把外部绝对路径映射到只读 URL。
        self._register_spec_paths(spec)

    def _register_spec_paths(self, spec):
        """把 spec 引用的外部素材登记为预览可读，白名单精确到文件级。"""
        root = os.path.realpath(self._spec_root())
        project = os.path.realpath(ROOT_DIR)
        candidates = []
        for sc in (spec.get("scenes") or []):
            if not isinstance(sc, dict):
                continue
            for key in ("video", "narration", "image", "poster", "audio"):
                val = sc.get(key)
                if isinstance(val, str):
                    candidates.append(val)
                elif isinstance(val, dict) and isinstance(val.get("src"), str):
                    candidates.append(val["src"])
        audio = spec.get("audio")
        if isinstance(audio, dict) and isinstance(audio.get("src"), str):
            candidates.append(audio["src"])
        for p in candidates:
            full = p if os.path.isabs(p) else os.path.join(root, p)
            full = os.path.realpath(urllib.parse.unquote(full))
            if not os.path.isfile(full):
                continue
            if full.startswith(project + os.sep):
                continue  # 项目根内走 ROOT_ASSET_PREFIX，无需额外登记
            # spec 可能在项目外（/tmp 等），此时连相对素材目录里的绝对
            # 路径也要登记，否则预览页会把 /tmp/... 当成 URL 路径而 404。
            self.external_files.add(full)

    def _thumbs_dir(self):
        d = os.path.join(self._spec_root(), "__thumbs__")
        os.makedirs(d, exist_ok=True)
        return d

    def build_thumbs(self, raw_path, n=10, w=480, h=270):
        """从源视频抽 n 帧生成缩略图，返回只读服务 URL 列表（带缓存）。

        raw_path 是 spec 里记录的绝对 / 相对路径（与 _register_spec_paths
        同源登记）。仅在白名单内且文件存在时才抽帧，避免暴露任意磁盘。
        """
        import hashlib
        import subprocess
        raw = urllib.parse.unquote(raw_path)
        root = os.path.realpath(ROOT_DIR)
        spec_root = os.path.realpath(self._spec_root())
        # spec 里 video 可能是相对路径（hf_import 写的是相对 spec_root 的
        # 地址，如 "imported/<批次>/kpl.mp4"）。realpath 会把相对地址误当成
        # 当前工作目录的子路径，导致文件找不到、缩略图整条失败。这里优先按
        # spec_root 解析，回退 ROOT_DIR，与 _register_spec_paths 同源。
        if os.path.isabs(raw):
            full = os.path.realpath(raw)
        else:
            cand_spec = os.path.realpath(os.path.join(spec_root, raw))
            cand_root = os.path.realpath(os.path.join(root, raw))
            full = cand_spec if os.path.isfile(cand_spec) else cand_root
        allowed = (full.startswith(root + os.sep) or
                   full.startswith(spec_root + os.sep) or
                   full in self.external_files)
        if not allowed or not os.path.isfile(full):
            raise RuntimeError("缩略图素材不可访问: " + raw_path)
        key = "{}|{}|{}|{}".format(full, n, w, h)
        hsh = hashlib.sha1(key.encode("utf-8")).hexdigest()[:16]
        cache = os.path.join(self._thumbs_dir(), hsh)
        os.makedirs(cache, exist_ok=True)
        urls = []
        pending = []
        for i in range(n):
            jp = os.path.join(cache, "{}.jpg".format(i))
            if os.path.isfile(jp):
                urls.append(THUMBS_PREFIX + hsh + "/" + "{}.jpg".format(i))
            else:
                pending.append((i, jp))
        if pending:
            dur = 0.0
            try:
                out = subprocess.run(
                    ["ffprobe", "-v", "error", "-show_entries",
                     "format=duration", "-of",
                     "default=nokey=1:noprint_wrappers=1", full],
                    capture_output=True, text=True, timeout=30)
                dur = float(out.stdout.strip() or 0)
            except Exception:
                dur = 0.0
            for i, jp in pending:
                tt = (dur * (i + 0.5) / n) if dur > 0 else i * 0.2
                try:
                    subprocess.run(
                        ["ffmpeg", "-v", "error", "-y", "-ss",
                         "{:.3f}".format(tt), "-i", full, "-frames:v", "1",
                         "-q:v", "2",
                         "-vf",
                         "scale={}:{}:force_original_aspect_ratio=increase,"
                         "crop={}:{}".format(w, h, w, h), jp],
                        capture_output=True, timeout=30)
                except Exception:
                    pass
                if os.path.isfile(jp):
                    urls.append(THUMBS_PREFIX + hsh + "/" +
                                "{}.jpg".format(i))
        return urls

    def _save_uploads(self, ctype, body):
        """multipart/form-data → spec_dir/imported/<批次>/，返回可导入目录。

        浏览器文件选择器拿不到本机绝对路径，所以把选中的素材复制到 spec
        目录内（烘焙 / 预览都能读到）。超大素材请用「本机目录路径」方式，
        那条链路零拷贝、不占内存。
        """
        import hf_import
        m = re.search(r'boundary=(?:"([^"]+)"|([^;\s]+))', ctype or "")
        if not m:
            raise RuntimeError("上传缺少 multipart boundary")
        boundary = (m.group(1) or m.group(2)).strip().encode("utf-8")
        allowed = (set(hf_import.VIDEO_EXTS) | set(hf_import.AUDIO_EXTS) |
                   {".srt", ".vtt", ".json"})
        base = os.path.join(self._spec_root(), "imported")
        os.makedirs(base, exist_ok=True)
        out_dir = tempfile.mkdtemp(
            prefix=time.strftime("%Y%m%d-%H%M%S") + "-", dir=base)
        files, total = [], 0
        for part in body.split(b"--" + boundary):
            if not part or part.startswith(b"--"):
                continue
            if part.startswith(b"\r\n"):
                part = part[2:]
            head, sep, data = part.partition(b"\r\n\r\n")
            if not sep or not data:
                continue
            if data.endswith(b"\r\n"):
                data = data[:-2]
            fn = re.search(rb'filename="([^"]*)"', head)
            if not fn:
                continue
            name = os.path.basename(fn.group(1).decode(
                "utf-8", "replace").replace("\\", "/"))
            if not name or name.startswith("."):
                continue
            if os.path.splitext(name)[1].lower() not in allowed:
                continue
            dest = os.path.join(out_dir, name)
            stem, ext = os.path.splitext(name)
            n = 1
            while os.path.exists(dest):
                dest = os.path.join(out_dir, f"{stem}-{n}{ext}")
                n += 1
            with open(dest, "wb") as f:
                f.write(data)
            files.append({"name": os.path.basename(dest), "size": len(data)})
            total += len(data)
        if not files:
            try:
                os.rmdir(out_dir)
            except OSError:
                pass
            raise RuntimeError("没有可导入的音视频 / 字幕文件")
        return {"dir": out_dir, "files": files, "count": len(files),
                "bytes": total}

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

    # ---- Agent / ASR / 素材导入（对位 Remotion「Make videos agentically」）----
    def _spec_root(self):
        return os.path.dirname(self.spec_path) if self.spec_path else self.spec_dir

    def _apply_spec(self, spec):
        """外部（Agent / import / LLM）产出的 spec 统一走校验 → 写回 → 重组。"""
        import hf_agent
        hf_agent.validate_spec(spec, self._spec_root())
        self._write_spec(spec)
        return self._compose()

    def _run_tts(self):
        if self.spec_path is None:
            raise RuntimeError("未绑定 spec 文件")
        proc = subprocess.run(
            [sys.executable, os.path.join(TOOLS_DIR, "hf_tts.py"),
             self.spec_path, "-o", self.spec_path],
            capture_output=True, text=True, timeout=600)
        if proc.returncode != 0:
            raise RuntimeError(proc.stderr[-400:] or "TTS 失败")
        self.current_spec = self._load_spec()
        return {"generated": proc.stdout.strip()[-200:],
                "spec": self.current_spec}

    def _run_import(self, opts):
        import hf_import
        raw = str(opts.get("dir") or "").strip()
        # 容忍从 Finder / 终端粘贴时带上的引号
        if len(raw) >= 2 and raw[0] == raw[-1] and raw[0] in "\"'“”‘’":
            raw = raw[1:-1].strip()
        src = os.path.expanduser(raw)
        # 相对路径按 spec 目录解析，避免受服务进程 CWD 影响（与缩略图同源）。
        if src and not os.path.isabs(src):
            cand = os.path.join(self._spec_root(), src)
            if os.path.exists(cand):
                src = cand
        if not src:
            raise RuntimeError(
                "请提供素材路径，例如「导入 ~/Downloads/2026-11-15」或"
                "「导入 ~/Movies/kpl.mp4」，也可以点「导入素材」按钮选择。")
        if not os.path.exists(src):
            raise RuntimeError(
                f"路径不存在：{src}，请检查拼写，或改用「导入素材」按钮选择"
                f"文件 / 目录")
        if not os.path.isfile(src) and not os.path.isdir(src):
            raise RuntimeError(f"既不是文件也不是目录：{src}")
        captions = os.path.expanduser(str(opts.get("captions") or ""))
        spec, notes = hf_import.build_spec(
            src, self._spec_root(),
            fmt=opts.get("format") or "16:9",
            fps=int(opts.get("fps") or 30),
            asr=bool(opts.get("asr")),
            captions_file=captions,
            endcard=bool(opts.get("endcard")),
            brand=opts.get("brand") or "xmedia-kit",
            tagline=opts.get("tagline") or "",
            bg=opts.get("bg") or "#000000",
            sfx=bool(opts.get("sfx")),
            provider=opts.get("provider") or "auto")
        # 记住「上次导入位置」：单文件时记它所在目录，方便下次复用。
        self.last_import_dir = (os.path.dirname(src)
                                if os.path.isfile(src) else src)
        # 合并策略：默认「替换」——时间轴只留本次导入的素材，避免上一次
        # 导入的内容残留在新素材前面；只有显式 append=true（或旧参数
        # replace=false）才追加到末尾。
        do_append = bool(opts.get("append")) or opts.get("replace") is False
        if do_append:
            dup = self._duplicate_note(spec)
            if dup:
                notes.append(dup)
            merged = self._append_import(spec)
        else:
            merged = self._replace_import(spec)
        # 导入前快照，供「撤销导入」回滚（替换是破坏性的，必须留后路）。
        import copy
        prev = copy.deepcopy(self.current_spec) if self.current_spec else None
        composed = self._apply_spec(merged)
        if prev and (prev.get("scenes") or []):
            self._undo_spec = prev
            self._undo_label = "{} 场景 · {:.2f}s".format(
                len(prev.get("scenes") or []),
                sum(float(s.get("duration") or 0)
                    for s in prev.get("scenes") or []))
        else:
            self._undo_spec = None
            self._undo_label = ""
        if not do_append:
            notes.append("已替换时间轴（仅保留本次导入）；勾选「追加到当前"
                         "时间轴」可保留原有内容")
        return {"dir": src, "notes": notes,
                "single_file": os.path.isfile(src),
                "mode": "append" if do_append else "replace",
                "can_undo": bool(self._undo_spec),
                "composed": composed,
                "spec": self.current_spec}

    def _duplicate_note(self, new_spec):
        """导入的素材与现有场景同文件时返回提示文案，否则空串。"""
        try:
            existing = set()
            for sc in (self.current_spec or {}).get("scenes") or []:
                v = sc.get("video")
                if isinstance(v, str) and v:
                    p = v if os.path.isabs(v) else os.path.join(
                        self._spec_root(), v)
                    existing.add(os.path.realpath(p))
            for sc in new_spec.get("scenes") or []:
                v = sc.get("video")
                if not isinstance(v, str) or not v:
                    continue
                p = v if os.path.isabs(v) else os.path.join(
                    self._spec_root(), v)
                if os.path.realpath(p) in existing:
                    return ("提示：该素材已在时间轴上，本次为追加；若要只保留"
                            "本次导入，取消勾选「追加到当前时间轴」。")
        except Exception:
            pass
        return ""

    def _append_import(self, new_spec):
        """非破坏性导入：把导入的场景追加到当前时间轴末尾，保留当前
        的 composition / 背景 / 转场 / SFX 等全局设置；导入的全局字幕段
        按当前总时长偏移后并入。当前无内容时直接采用导入结果。"""
        import copy
        import hf_asr
        base = copy.deepcopy(self.current_spec or {})
        if not (base.get("scenes")):
            return new_spec
        base.setdefault("scenes", [])
        cur_total = 0.0
        if self.current_html:
            try:
                cur_total = parse_total(self.current_html,
                                        parse_scenes(self.current_html))
            except Exception:
                cur_total = 0.0
        caps = new_spec.get("captions")
        if isinstance(caps, dict) and caps.get("segments"):
            shifted = []
            for seg in caps["segments"]:
                s = dict(seg)
                s["start"] = round((s.get("start") or 0) + cur_total, 3)
                s["end"] = round((s.get("end") or 0) + cur_total, 3)
                shifted.append(s)
            existing = base.get("captions", {}) or {}
            merged_segs = list(existing.get("segments", [])) + shifted
            base["captions"] = {"enabled": True,
                                "segments": hf_asr.normalize_segments(merged_segs)}
        base["scenes"].extend(new_spec.get("scenes", []))
        return base

    def _replace_import(self, new_spec):
        """破坏性导入：整条时间轴换成本次素材（默认行为）。

        只保留导入 spec 自身的 composition / 背景 / 转场 / SFX，避免上一
        次导入的画面、字幕、BGM 残留到新素材前面。"""
        import copy
        spec = copy.deepcopy(new_spec or {})
        spec.setdefault("schema_version", "1.0")
        spec.setdefault("scenes", [])
        return spec

    def _undo_import(self):
        """把时间轴回滚到最近一次导入前的状态。"""
        if not self._undo_spec:
            raise RuntimeError("没有可撤销的导入")
        spec = self._undo_spec
        self._undo_spec = None
        label = self._undo_label
        self._undo_label = ""
        return {"restored": label,
                "composed": self._apply_spec(spec),
                "spec": self.current_spec}

    def _run_asr(self, opts=None):
        """对当前 spec 里每个 video 场景跑 ASR，按累计起点合成全局字幕段。"""
        import hf_asr
        opts = opts or {}
        spec = copy.deepcopy(self.current_spec or {})
        cursor, segs, notes = 0.0, [], []
        for idx, sc in enumerate(spec.get("scenes") or []):
            try:
                dur = float(sc.get("duration", 3))
            except (TypeError, ValueError):
                dur = 3.0
            video = sc.get("video")
            if video:
                path = (video if os.path.isabs(video) else
                        os.path.join(self._spec_root(), video))
                try:
                    res = hf_asr.transcribe_with_fallback(
                        path, provider=opts.get("provider") or "auto",
                        srt_path=opts.get("srt") or None)
                    for seg in res["segments"]:
                        segs.append({"start": round(cursor + seg["start"], 3),
                                     "end": round(cursor + seg["end"], 3),
                                     "text": seg["text"]})
                    notes.append(f"scene-{idx}: {len(res['segments'])} 段"
                                 f"（{res['provider']}）")
                except Exception as e:
                    notes.append(f"scene-{idx} 跳过：{e}")
            cursor += dur
        if not segs:
            # 把每个场景的失败原因带回给前端，而不是只给一句笼统的报错
            detail = "；".join(notes)[-400:] if notes else "当前时间轴没有视频素材"
            hint = ""
            if "未安装" in detail or "没有可用的 ASR" in detail:
                hint = "。可执行 pip install faster-whisper，或给素材配同名 .srt"
            return {"ok": False, "notes": notes,
                    "error": f"没有产出字幕段：{detail}{hint}"}
        spec.setdefault("captions", {})["enabled"] = True
        spec["captions"]["segments"] = hf_asr.normalize_segments(
            segs, cursor + 3.0)
        return {"ok": True, "segments": len(spec["captions"]["segments"]),
                "notes": notes, "composed": self._apply_spec(spec),
                "spec": self.current_spec}

    def _run_agent(self, message):
        import hf_agent
        result = hf_agent.handle(message, self.current_spec, self._spec_root())
        out = {"reply": result.get("reply", ""), "mode": result.get("mode"),
               "action": result.get("action")}
        if "spec" in result:
            out["composed"] = self._apply_spec(result["spec"])
            out["spec"] = self.current_spec
        action, args = result.get("action"), result.get("args") or {}
        try:
            if action == "tts":
                out["tts"] = self._run_tts()
                out["composed"] = self._compose()
            elif action == "bake":
                out["job"] = self._start_bake()
            elif action == "import":
                out["import"] = self._run_import(args)
            elif action == "asr":
                out["asr"] = self._run_asr(args)
            elif action == "recompose":
                out["composed"] = self._compose()
        except Exception as e:
            emsg = str(e)
            # 目录不存在属于「用户输入问题」，回成普通助手提示，避免前端弹出
            # 红色「请求失败」；其余（compose 失败等）仍作为 error 上报。
            if action == "import" and emsg.startswith("素材目录不存在"):
                out["reply"] = emsg
                out["mode"] = "help"
            else:
                out["error"] = emsg
                out.setdefault("reply", "")
                out["reply"] += f"（动作执行失败：{emsg}）"
        return out


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
                            "agent": {"llm": bool(os.environ.get(
                                "XMEDIA_LLM_ENDPOINT")), "mcp": True},
                            "import": {
                                "last_dir": srv.last_import_dir,
                                "spec_dir": srv._spec_root(),
                                "upload_max_mb":
                                    MAX_UPLOAD_BYTES // (1024 * 1024),
                                "can_undo": bool(srv._undo_spec),
                                "undo_label": srv._undo_label},
                            "findings": [{"level": lv, "message": msg}
                                         for lv, msg in findings]})
            elif path == PREVIEW_MIX_URL:
                self._serve_mix()
            elif path == "/preview":
                mix_url = PREVIEW_MIX_URL if srv.preview_mix() else ""
                self._send(200, inject(srv.current_html, for_http=True,
                                       mix_url=mix_url,
                                       external_files=srv.external_files),
                           "text/html; charset=utf-8")
            elif path == "/api/job":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                try:
                    self._json(srv._job_status(int(q["id"][0])))
                except (RuntimeError, KeyError, ValueError) as e:
                    self._json({"error": str(e)}, 400)
            elif path == "/api/thumbs":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(
                    self.path).query)
                raw = (q.get("path") or [""])[0]
                try:
                    n = max(1, min(24, int((q.get("n") or ["8"])[0])))
                    w = max(16, min(480, int((q.get("w") or ["120"])[0])))
                    h = max(16, min(480, int((q.get("h") or ["68"])[0])))
                    frames = srv.build_thumbs(raw, n=n, w=w, h=h)
                    self._json({"ok": True, "frames": frames,
                                "count": len(frames)})
                except Exception as e:
                    self._json({"ok": False, "error": str(e), "frames": []}, 400)
            elif path.startswith(THUMBS_PREFIX):
                self._serve_thumb(path)
            else:
                self._serve_asset(path)

        def do_POST(self):
            path = urllib.parse.urlparse(self.path).path
            srv = server_ref
            length = int(self.headers.get("Content-Length", 0))
            # 文件选择器走 multipart，不是 JSON——先于 JSON 解析单独处理。
            if path == "/api/import/upload":
                if length > MAX_UPLOAD_BYTES:
                    self._json({"error": "上传超过 512MB，请改用「本机目录路径」"
                                         "方式导入（零拷贝）"}, 400)
                    return
                try:
                    self._json(srv._save_uploads(
                        self.headers.get("Content-Type", ""),
                        self.rfile.read(length)))
                except (RuntimeError, OSError, ValueError) as e:
                    self._json({"error": str(e)}, 400)
                return
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
                elif path == "/api/asr":
                    import hf_asr
                    media = os.path.expanduser(str(
                        body.get("path") or body.get("media") or ""))
                    if not media:
                        raise RuntimeError("缺少 path（音视频素材或 SRT 路径）")
                    res = hf_asr.transcribe(
                        media, provider=body.get("provider") or "auto",
                        srt_path=body.get("srt") or None)
                    if body.get("apply"):
                        spec = copy.deepcopy(srv.current_spec or {})
                        spec.setdefault("captions", {})["enabled"] = True
                        spec["captions"]["segments"] = res["segments"]
                        res["composed"] = srv._apply_spec(spec)
                    self._json(res)
                elif path == "/api/import":
                    self._json(srv._run_import(body))
                elif path == "/api/import/undo":
                    self._json(srv._undo_import())
                elif path == "/api/agent":
                    self._json(srv._run_agent(str(body.get("message") or "")))
                else:
                    self._json({"error": "unknown endpoint"}, 404)
            except (RuntimeError, ValueError, OSError, KeyError) as e:
                self._json({"error": str(e)}, 400)

        def _serve_mix(self):
            """单条预览混音：支持 Range，Safari 的 audio seek 依赖 206。"""
            data = server_ref.preview_mix()
            if not data:
                self._send(404, "preview mix unavailable", "text/plain")
                return
            status, chunk, content_range = 200, data, None
            rng = self.headers.get("Range", "")
            if rng.startswith("bytes="):
                m = re.match(r"(\d*)-(\d*)", rng[6:].split(",")[0].strip())
                if m:
                    start = int(m.group(1)) if m.group(1) else 0
                    end = int(m.group(2)) if m.group(2) else len(data) - 1
                    end = min(end, len(data) - 1)
                    if start <= end and start < len(data):
                        chunk = data[start:end + 1]
                        status = 206
                        content_range = f"bytes {start}-{end}/{len(data)}"
            self.send_response(status)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(chunk)))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Cache-Control", "no-store")
            if content_range:
                self.send_header("Content-Range", content_range)
            self.end_headers()
            try:
                self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                pass

        def _serve_thumb(self, path):
            srv = server_ref
            rel = urllib.parse.unquote(path.lstrip("/"))
            # THUMBS_PREFIX 已带尾部斜杠；rel 形如 <hsh>/<i>.jpg
            rel = rel[len(THUMBS_PREFIX.lstrip("/")):]
            parts = rel.split("/")
            if len(parts) != 2:
                self._send(404, "not found", "text/plain")
                return
            hsh, fname = parts
            if not re.fullmatch(r"[0-9a-f]{16}", hsh) or \
               not re.fullmatch(r"\d+\.(jpg|png)", fname):
                self._send(404, "not found", "text/plain")
                return
            full = os.path.realpath(
                os.path.join(srv._thumbs_dir(), hsh, fname))
            base = os.path.realpath(srv._thumbs_dir())
            if full != os.path.join(base, hsh, fname) or \
               not full.startswith(base + os.sep) or not os.path.isfile(full):
                self._send(404, "not found", "text/plain")
                return
            with open(full, "rb") as f:
                self._send(200, f.read(), THUMB_EXTS.get(
                    os.path.splitext(full)[1].lower(), "image/jpeg"))

        def _serve_asset(self, path):
            srv = server_ref
            rel = urllib.parse.unquote(path.lstrip("/"))
            ext = os.path.splitext(rel)[1].lower()
            if ext not in FILE_EXTS:
                self._send(404, "not found", "text/plain")
                return
            media_prefix = EXTERNAL_ASSET_PREFIX.lstrip("/")
            root_prefix = ROOT_ASSET_PREFIX.lstrip("/")
            if rel.startswith(media_prefix):
                # 外部导入素材：只允许 spec 显式登记过的文件（精确到文件级）。
                full = os.path.realpath("/" + rel[len(media_prefix):])
                if full not in srv.external_files:
                    self._send(404, "not found", "text/plain")
                    return
            else:
                # 项目根只读映射 / spec 目录相对路径。
                base = (os.path.realpath(ROOT_DIR)
                        if rel.startswith(root_prefix) else
                        os.path.realpath(srv.spec_dir))
                if rel.startswith(root_prefix):
                    rel = rel[len(root_prefix):]
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
                     "audio/flac" if ext == ".flac" else
                     "audio/ogg" if ext == ".ogg" else
                     "video/mp4" if ext in (".mp4", ".m4v") else
                     "video/webm" if ext == ".webm" else
                     "video/quicktime" if ext == ".mov" else
                     "video/x-matroska" if ext == ".mkv" else
                     "video/x-msvideo" if ext == ".avi" else
                     "image/png" if ext == ".png" else
                     "image/jpeg" if ext in (".jpg", ".jpeg") else
                     "image/webp" if ext == ".webp" else
                     "image/svg+xml" if ext == ".svg" else
                     "text/css; charset=utf-8" if ext == ".css" else
                     "text/javascript; charset=utf-8" if ext == ".js" else
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
