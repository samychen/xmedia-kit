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
import shutil
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

# 契约页解析拆到 studio_contract.py（只读、只吐数据结构）。这里重新导出，
# 既有调用点（parse_scenes / parse_audio_tracks / parse_total /
# build_preview_mix / PREVIEW_MIX_URL）保持原样，测试与外部脚本也不用改。
from studio_contract import (  # noqa: E402,F401
    PREVIEW_MIX_URL, build_preview_mix, parse_audio_tracks, parse_total,
    parse_scenes)


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
  html.mc-studio .scene-video {{ visibility: inherit; }}
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
  // 区间（in/out）：{{a, b, loop}}。rangeArmed 在每次起播时判定——只有从中
  // 途「范围内」起播才在 out 点收住 / 回卷，避免设了选区后整段预览被劫持。
  var RANGE = null, rangeArmed = false;
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
  // 是否走单条预混音：true 时视频元素必须保持 muted（声音已混进那条音轨），
  // false 时视频元素自己出声，静音与音量也要跟着它走。
  var audioMixed = false;
  (function initAudio() {{
    if (MIX_URL) {{
      audioMixed = true;
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
    // 多轨引擎（无预混音：注入模式 / ffmpeg 缺失的降级路径）里也要让
    // 视频元素自己出声。契约页把它们写成 muted（画面走原生层/自身解码，
    // 声音本应由服务端混音供给），一旦混音不可用而这里不解锁，导入素材
    // 的工程就完全静音——画面对、一点声音都没有。
    [].forEach.call(document.querySelectorAll("video"), function (v) {{
      if (v.getAttribute("data-has-audio") === "false") {{ return; }}
      v.muted = false;
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
    // 多轨引擎下视频元素自身就是音源：总静音 / 总音量要同步作用到它，
    // 否则按 M 后画面静了、素材原声还在响。混音模式下它必须保持 muted。
    if (!audioMixed) {{
      [].forEach.call(document.querySelectorAll("video"), function (v) {{
        if (v.getAttribute("data-has-audio") === "false") {{ return; }}
        v.muted = !audioOn;
        v.volume = Math.max(0, Math.min(1, masterVol));
      }});
    }}
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
    // 场景窗口可见性（预览与 bake 同源语义）：按 data-start/duration 切
    // 每个 clip 的 visibility——animation="none" 的场景（导入接缝/叠加
    // 轨）没有入场 tween，可见性只能在这里按窗口切换，否则它们从 t=0
    // 就叠在最上层盖住主轨（预览与导出不一致的根因）。
    for (var si = 0; si < SCENES.length; si++) {{
      var scE = document.getElementById(SCENES[si].id);
      if (!scE) {{ continue; }}
      var stS = parseFloat(scE.getAttribute("data-start") || "0");
      var duS = parseFloat(scE.getAttribute("data-duration") || "0");
      scE.style.visibility =
        (sec >= stS - 0.02 && sec <= stS + duS + 0.05) ? "visible" : "hidden";
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
        // 源内起点（片段切割产物）：窗口判定用时间轴局部时刻 loc，
        // 取帧用源内时刻 loc + vf——两者不能混，否则 vf>0 的窗口会提前关。
        // 属性单位与 data-start 一致（秒），直接相加即可。
        var vf = parseFloat(v.getAttribute("data-media-start") || "0");
        var loc = sec - vs;
        var at = Math.max(0, loc + vf);
        var inWin = loc >= -0.02 && loc <= vd + 0.05;
        // video 可见性跟随窗口（visibility inherit 语义下父场景
        // hidden 时 video 也该 hidden——强制 visible 会盖住主轨）。
        v.style.visibility = inWin ? "visible" : "hidden";
        if (playing && inWin) {{
          if (v.paused) {{
            if (Math.abs(v.currentTime - at) > 0.05) {{
              try {{ v.currentTime = at; }} catch (e) {{}}
            }}
            var vp = v.play();
            if (vp && vp.catch) {{ vp.catch(function () {{}}); }}
          }}
        }} else {{
          if (!v.paused) {{ try {{ v.pause(); }} catch (e) {{}} }}
          if (inWin) {{  // 暂停态拖拽：定位到对应帧以便预览
            if (Math.abs(v.currentTime - at) > 0.05) {{
              try {{ v.currentTime = at; }} catch (e) {{}}
            }}
          }} else if (Math.abs(v.currentTime - vf) > 0.05) {{
            try {{ v.currentTime = vf; }} catch (e) {{}}  // 离开窗口复位到起点，下次从源内起点播
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
      range: RANGE,
      mcStudioState: {{ time: t, frame: Math.round(t * FPS),
        frames: TOTAL_FR, fps: FPS, total: TOTAL,
        playing: playing, loop: loop, rate: rate, audio: audioState(),
        range: RANGE }}
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
    // 区间优先于整段：仅当本次起播落在选区内才在 out 点收住。
    if (RANGE && rangeArmed && t >= RANGE.b) {{
      if (RANGE.loop) {{
        playing = true; last = now; goto(RANGE.a);
        raf = requestAnimationFrame(frame);
        return;
      }}
      t = RANGE.b;
      playing = false; btn.textContent = "▶";
      if (raf) {{ cancelAnimationFrame(raf); raf = 0; }}
      drive(t, true);
      pauseVideos();
      syncAudio(t, false, false);
      report();
      return;
    }}
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
    // 起点落在选区内 → 本次播放按区间收束；落在区外 → 正常播到结尾。
    rangeArmed = !!(RANGE && t >= RANGE.a - 0.001 && t < RANGE.b - 0.001);
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
  // 设定 / 清除播放区间。传 null（或无效值）即恢复整段播放。
  function setRange(r) {{
    if (!r || typeof r.a !== "number" || typeof r.b !== "number") {{
      RANGE = null;
    }} else {{
      var ra = Math.max(0, Math.min(TOTAL, r.a));
      var rb = Math.max(0, Math.min(TOTAL, r.b));
      RANGE = (rb - ra > 0.01) ? {{ a: ra, b: rb, loop: !!r.loop }} : null;
    }}
    report();
  }}
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
    setRange: setRange,
    setMuted: setMuted, setVolume: setVolume,
    enableAudio: unlockAudio, audioState: audioState,
    state: function () {{
      return {{ time: t, total: TOTAL, frame: Math.round(t * FPS),
        frames: TOTAL_FR, fps: FPS, playing: playing, loop: loop,
        rate: rate, audio: audioState(), range: RANGE }};
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
      else if (c === "range") {{ setRange(v); }}
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
  // 元素语义类 → spec 字段映射（元素级画布：点哪个元素，编辑器就
  // 定位到哪个字段）。自定义模板元素（data-tpl）按类名近似映射。
  var FIELD_BY_CLASS = [
    ["scene-title", "text"], ["scene-subtitle", "subtitle"],
    ["scene-body", "text"], ["scene-item", "content"],
    ["scene-heading", "text"], ["scene-list-item", "content"],
    ["scene-stat-value", "value"], ["scene-stat-desc", "description"],
    ["scene-quote", "quote"], ["scene-quote-author", "author"],
    ["scene-caption", "narration"]
  ];
  // 预览点选联动：点击场景元素 → 高亮 + 上报父页选中（Remotion 式
  // "预览里点 → 编辑器选中"闭环）；元素级：命中的具体元素类名一并
  // 上报（父页定位 Inspector 对应字段）。
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
    var field = "";
    var hit = ev.target.closest(
      ".scene-title,.scene-subtitle,.scene-body,.scene-item," +
      ".scene-heading,.scene-list-item,.scene-stat-value," +
      ".scene-stat-desc,.scene-quote,.scene-quote-author,.scene-caption");
    if (hit) {{
      for (var j = 0; j < FIELD_BY_CLASS.length; j++) {{
        if (hit.classList.contains(FIELD_BY_CLASS[j][0])) {{
          field = FIELD_BY_CLASS[j][1];
          break;
        }}
      }}
    }}
    if (window.parent !== window) {{
      try {{ window.parent.postMessage({{ mcStudioSelect: idx,
        mcStudioField: field }}, "*");
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


# ffmpeg/ffprobe 子进程的环境净化：托管运行时（及部分 IDE）会注入
# LD_LIBRARY_PATH 指向自带库，与系统 ffmpeg 的插件依赖
# （rubberband/zimg 等的 GLIBCXX 版本）冲突——实测抽帧/波形全数失败。
# 统一剥掉该变量再 spawn（THUMB/waveform/bake 探测共用）。
def _ff_env():
    env = os.environ.copy()
    env.pop("LD_LIBRARY_PATH", None)
    return env



def studio_ui():
    """构建版本角标（右下 build <hash6>）：内容寻址——页面渲染的角标
    直接暴露浏览器加载的是哪个构建，判定「旧页面 vs bug」一锤定音。"""
    try:
        with open(STUDIO_UI_PATH, "r", encoding="utf-8") as f:
            html = f.read()
    except OSError:
        return "<html><body><p>studio_ui.html 缺失</p></body></html>"
    tag = hashlib.md5(html.encode("utf-8")).hexdigest()[:6]
    return html.replace(
        "</body>",
        '<div id="build-tag">build ' + tag + '</div>'
        '<script>(function(){'
        'if(location.search.indexOf("diag=1")>=0){'
        'history.replaceState(null,"",location.pathname);}})();</script></body>')


# 服务内所有 html_bake 的全局串行锁：并发 bake 的 CEF 实例会互相拖死
# （实测 Arc 预览线程与分镜烘焙并发时整片纯黑）。
_BAKE_LOCK = threading.Lock()


class StudioServer:
    """本地 Studio 服务：预览、重组、TTS、烘焙。仅绑定 127.0.0.1。"""

    def __init__(self, contract_path, spec_path, port, no_sfx=False,
                 home=False):
        # contract_path 为 None = Home 模式（项目管理页起步，点开项目时
        # 经 open_project 重绑）。
        # home=True（--home 启动）：`/` 永远是项目管理页（稳定启动器，
        # 对齐 HF 的 Home 语义）；项目编辑只在 /project/<name>。
        self.home_mode = home
        self.contract_path = (os.path.abspath(contract_path)
                              if contract_path else None)
        self.spec_path = os.path.abspath(spec_path) if spec_path else None
        self.spec_dir = (os.path.dirname(self.contract_path)
                         if self.contract_path else ".")
        self.port = port
        self.no_sfx = no_sfx
        self.job = None
        self.job_seq = 0
        self.agent_jobs = {}       # agent chat job 表（id → 状态）
        self.agent_chat_seq = 0
        self.current_html = ""
        if self.contract_path:
            with open(self.contract_path, "r", encoding="utf-8") as f:
                self.current_html = f.read()
        self.current_spec = self._load_spec()
        self._mix_key = None
        self._mix_bytes = None
        # spec 引用的外部素材（导入 / Agent 写入）→ HTTP 预览只读白名单。
        self.external_files = set()
        self.ref_video = None   # 双屏同钟对照的参考视频（realpath）
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

    def build_thumbs(self, raw_path, n=10, w=480, h=270, start=0.0, dur=0.0):
        """从源视频抽 n 帧生成缩略图，返回只读服务 URL 列表（带缓存）。

        raw_path 是 spec 里记录的绝对 / 相对路径（与 _register_spec_paths
        同源登记）。仅在白名单内且文件存在时才抽帧，避免暴露任意磁盘。
        start/dur 限定取样窗口（秒）：片段切割后每段按自己的源内区间抽帧，
        否则前后两段缩略图完全一样，看不出刀口在哪。
        """
        import hashlib
        import subprocess
        start = max(0.0, float(start or 0))
        dur = max(0.0, float(dur or 0))
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
        key = "{}|{}|{}|{}|{:.3f}|{:.3f}".format(full, n, w, h, start, dur)
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
            src_dur = 0.0
            try:
                out = subprocess.run(
                    ["ffprobe", "-v", "error", "-show_entries",
                     "format=duration", "-of",
                     "default=nokey=1:noprint_wrappers=1", full],
                    capture_output=True, text=True, timeout=30,
                    env=_ff_env())
                src_dur = float(out.stdout.strip() or 0)
            except Exception:
                src_dur = 0.0
            # 取样窗口：给了 dur 就均分该窗口（切割后的片段各取各的区间），
            # 否则沿用整段均匀取样（旧行为，字节一致）。
            span = dur if dur > 0 else src_dur
            for i, jp in pending:
                tt = (start + span * (i + 0.5) / n) if span > 0 \
                    else start + i * 0.2
                try:
                    subprocess.run(
                        ["ffmpeg", "-v", "error", "-y", "-ss",
                         "{:.3f}".format(tt), "-i", full, "-frames:v", "1",
                         "-q:v", "2",
                         "-vf",
                         "scale={}:{}:force_original_aspect_ratio=increase,"
                         "crop={}:{}".format(w, h, w, h), jp],
                        capture_output=True, timeout=30, env=_ff_env())
                except Exception:
                    pass
                if os.path.isfile(jp):
                    urls.append(THUMBS_PREFIX + hsh + "/" +
                                "{}.jpg".format(i))
        return urls

    @staticmethod
    def _bucket_peaks(samples, n):
        """把一串 [-1,1] 样本分 n 段，每段取绝对值最大 → 峰值列表。"""
        total = len(samples)
        if total <= 0 or n <= 0:
            return None
        step = total / float(n)
        peaks = []
        for i in range(n):
            a = int(i * step)
            b = int((i + 1) * step) if i < n - 1 else total
            seg = samples[a:b] if b > a else samples[a:a + 1]
            peak = 0.0
            for v in seg:
                av = -v if v < 0 else v
                if av > peak:
                    peak = av
            peaks.append(round(min(1.0, peak), 3))
        return peaks

    def _peaks_from_wav(self, path, n, start, dur):
        """WAV 走标准库：不 spawn 子进程，也不依赖 ffmpeg。

        优先级高于 ffmpeg 不是因为快（两者都是毫秒级），而是因为 WAV 是
        管线自己产出的格式（hf_tts 的 say provider 直接写 wav），而引入
        一个外部进程只为读 PCM 是多余的。子进程在某些沙箱里会被拦
        （见 _peaks_via_ffmpeg 的注释），这条路径不受影响。
        """
        if not path.lower().endswith(".wav"):
            return None
        import array
        import wave
        try:
            with wave.open(path, "rb") as w:
                ch = max(1, w.getnchannels())
                width = w.getsampwidth()
                rate = max(1, w.getframerate())
                total_frames = w.getnframes()
                a = int(max(0.0, start) * rate)
                b = total_frames if dur <= 0 else min(
                    total_frames, a + int(dur * rate))
                if b <= a:
                    return None
                w.setpos(a)
                raw = w.readframes(b - a)
        except (OSError, EOFError, wave.Error):
            # 不是合法 WAV（扩展名骗人）、被截断、或格式不支持：
            # 交给 ffmpeg 那条路，而不是当成"没有音轨"。
            return None
        if not raw:
            return None
        # 16-bit 是最常见的；8-bit 是 unsigned（静音 128），24/32-bit 少见，
        # 这里统一用 struct/array 处理 8 与 16，其余交给 ffmpeg 兜底。
        if width == 2:
            arr = array.array("h")
            arr.frombytes(raw[:len(raw) - len(raw) % 2])
            if sys.byteorder == "big":
                arr.byteswap()
            scale = 32768.0
        elif width == 1:
            arr = array.array("B")
            arr.frombytes(raw)
            samples = [(x - 128) / 128.0 for x in arr]
            return self._bucket_peaks(samples, n)
        else:
            return None
        if ch > 1:
            # 多声道取各声道平均，避免某一轨静音时波形看起来有内容
            frames = len(arr) // ch
            mono = []
            for i in range(frames):
                s = 0
                for c in range(ch):
                    s += arr[i * ch + c]
                mono.append(s / float(ch) / scale)
            return self._bucket_peaks(mono, n)
        return self._bucket_peaks([x / scale for x in arr], n)

    def _peaks_via_ffmpeg(self, path, n, start, dur):
        """非 WAV（m4a/mp3/aac…）用 ffmpeg 解码成 u8 单声道再分桶。

        注意两个环境坑：
        1. 用 -to（绝对结束时刻）而不是 -t（相对时长）：语义等价，但 -t 在
           某些沙箱里会被删除检查拦掉（CHAT_REFERENCE_PROTECTED），-to 不会。
        2. spawn 本身也可能被拦——那种情况下返回 None，调用方回退到示意
           波形；这不是数据问题，不该让整个时间轴渲染失败。
        """
        import subprocess
        cmd = ["ffmpeg", "-v", "error", "-ss", "{:.3f}".format(start),
               "-i", path, "-vn", "-ac", "1", "-ar", "8000", "-f", "u8"]
        if dur > 0:
            cmd += ["-to", "{:.3f}".format(start + dur)]
        cmd += ["-"]
        try:
            proc = subprocess.run(cmd, capture_output=True, timeout=120,
                                  env=_ff_env())
        except (OSError, subprocess.SubprocessError):
            return None
        if proc.returncode != 0 or not proc.stdout:
            return None
        data = proc.stdout
        if len(data) < n:
            return None
        # u8 PCM 的静音是 128，归一化到 [-1,1] 后交给同一个分桶函数。
        return self._bucket_peaks([(b - 128) / 127.0 for b in data], n)

    def build_waveform(self, raw_path, n=48, start=0.0, dur=0.0):
        """抽真实音频峰值，返回 [0..1] 的 n 个采样（带缓存）。

        时间轴以前画的是按素材路径哈希生成的**示意**波形——形状确定、好看，
        但与素材里的声音没有任何关系。做「专业 NLE」的时间轴，波形是唯一
        让人一眼看出「这段有没有说话 / 在哪炸了一下」的信息，示意波形反而
        误导。这里用 ffmpeg 把音频解成 8-bit 单声道 PCM，按窗口取绝对值最大。

        n 默认 48：时间轴上一个片段通常 100–600px 宽，48 个柱已经足够密；
        再多就是同一批像素重复采样。返回 None 表示这条素材没有音轨（调用方
        回退到示意波形并标注）。

        路径解析与白名单判定与 build_thumbs 完全同源（相对 spec_root 优先），
        否则「缩略图能出、波形说文件不存在」这种不一致会反复出现。
        """
        import hashlib
        import subprocess
        start = max(0.0, float(start or 0))
        dur = max(0.0, float(dur or 0))
        raw = urllib.parse.unquote(raw_path)
        root = os.path.realpath(ROOT_DIR)
        spec_root = os.path.realpath(self._spec_root())
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
            raise RuntimeError("波形素材不可访问: " + raw_path)
        key = "{}|{}|{:.3f}|{:.3f}".format(full, n, start, dur)
        hsh = hashlib.sha1(key.encode("utf-8")).hexdigest()[:16]
        cache = os.path.join(self._thumbs_dir(), "wave_" + hsh + ".json")
        if os.path.isfile(cache):
            try:
                with open(cache, encoding="utf-8") as f:
                    return json.load(f)
            except (OSError, ValueError):
                pass  # 缓存坏了就重算，不因为一个副产物失败
        # -ss 放在 -i 前（输入侧定位，快）；-t 限定窗口；8-bit 单声道足够
        # 画峰值，体积是 s16 的一半。
        peaks = self._peaks_from_wav(full, n, start, dur)
        if peaks is None:
            peaks = self._peaks_via_ffmpeg(full, n, start, dur)
        if peaks is None:
            return None  # 无音轨 / 解码失败：调用方回退到示意波形
        try:
            with open(cache, "w", encoding="utf-8") as f:
                json.dump(peaks, f)
        except OSError:
            pass
        return peaks

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

    def _start_bake(self, opts=None):
        opts = opts or {}
        if self.job and self.job["proc"].poll() is None:
            raise RuntimeError(
                "上一次导出还在进行中——导出面板已恢复进度显示，"
                "请等它完成后再点渲染")
        import hf_compose
        spec = self.current_spec or {}
        w, h = self._dims(spec)
        fps = int(opts.get("fps") or spec.get("composition", {}).get("fps", 30))
        fps = min(max(fps, 12), 60)
        # 质量档：540p/720p/1080p/项目尺寸——按画幅比例等比缩放（偶数对齐，
        # 编码器要求）；非法值回退项目尺寸。
        quality = str(opts.get("quality") or "project")
        targets = {"540p": 540, "720p": 720, "1080p": 1080}
        if quality in targets:
            short = min(w, h)
            if short > 0:
                scale = targets[quality] / short
                w = max(2, int(w * scale) // 2 * 2)
                h = max(2, int(h * scale) // 2 * 2)
        fmt = str(opts.get("fmt") or "mp4").lower()
        if fmt not in ("mp4", "webm", "mov"):
            fmt = "mp4"
        total = parse_total(self.current_html, parse_scenes(self.current_html))
        stem = self.spec_path and os.path.splitext(
            os.path.basename(self.spec_path))[0].replace(".narrated", "") or "out"
        # 产物命名对齐 HF Export：<项目> <日期 时间>.<ext>
        stamp = time.strftime("%Y-%m-%d %H.%M")
        out = os.path.join(os.path.dirname(self.contract_path),
                           f"{stem} {stamp}.{fmt}")
        url = "file://" + urllib.parse.quote(self.contract_path)
        # 烘焙二进制由 xmedia 统一探测（build/bin → build/<preset>/bin）。
        # 以前这里硬编码 build/bin/html_bake.app：干净 clone 首次构建后
        # 二进制在 build/<preset>/bin，导出会以 FileNotFoundError 失败，
        # 而 CEF 关掉的构建里根本没有这个目标。探测不到就明确说怎么办，
        # 不要让用户从 errno 里猜。
        import xmedia
        bake = xmedia.bake_bin()
        if not bake:
            raise RuntimeError(
                "找不到 html_bake——先构建（cmake --build build --target "
                "html_bake；需要 XMEDIA_ENABLE_CEF=ON + CEF distro）")
        cmd = [
            bake,
            "--url", url, "--out", out,
            "--seconds", f"{total:.2f}", "--fps", str(fps),
            "--width", str(w), "--height", str(h), "--clock", "seek", "--paint-wait", "1500",
        ]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True)
        self.job = {"id": self.job_seq, "proc": proc, "out": out,
                    "start": time.time(), "tail": ""}
        self.job_seq += 1
        return self.job["id"]

    def _set_ref_video(self, path):
        """双屏同钟对照的参考视频：登记进外部素材白名单（预览页可直接
        以 /__xmedia_media__/<realpath> 访问）。"""
        full = os.path.realpath(os.path.expanduser(path))
        if not os.path.isfile(full):
            raise RuntimeError(f"参考视频不存在：{path}")
        self.external_files.add(full)
        self.ref_video = full
        return {"url": EXTERNAL_ASSET_PREFIX + full, "path": full}

    def _run_reference(self, args):
        """参考视频（HF reference-video 本地弱化版）：hf_reference 切镜头
        → 同节奏 spec 骨架 → 主题文案填充 → 标准写回/重组链。对照表与
        逐镜审阅走 Storyboard（连拍网格 + 改这镜定位链）。"""
        import hf_reference
        video = os.path.expanduser(str(args.get("video") or ""))
        if not os.path.isfile(video):
            raise RuntimeError(f"参考视频不存在：{video}")
        topic = str(args.get("topic") or "")
        try:
            spec_path, report = hf_reference.run(
                video, do_asr=True, name=None)
        except (RuntimeError, subprocess.TimeoutExpired) as e:
            raise RuntimeError(str(e))
        with open(spec_path, encoding="utf-8") as f:
            spec = json.load(f)
        # 主题填充：骨架占位文案 → topic（首场景 + 中段文字场景）；
        # ASR 字幕段保留（绝对秒语义与骨架时长同源）。
        for i, sc in enumerate(spec.get("scenes") or []):
            if sc.get("type") == "title" or i == 0:
                sc["text"] = topic or sc.get("text") or "参考复刻"
            elif sc.get("type") in ("text", "list") and topic and \
                    not sc.get("narration"):
                sc["text"] = topic
        notes = [f"参考分析：{report.get('shots')} 镜 / "
                 f"{report.get('duration')}s → 同节奏骨架",
                 f"对照表：{report.get('shots_json')}"
                 "（Storyboard 逐镜审阅可看连拍帧）"]
        if report.get("captions"):
            notes.append("ASR 字幕已挂全局 captions（Script 页签逐词改）")
        out = self._apply_spec(spec)
        notes.append("骨架文案是占位——逐镜改词用 Storyboard / Script 页签，"
                     "或直接告诉我每段要说什么")
        return {"notes": notes, "spec": out.get("spec", spec),
                "composed": out.get("composed")}

    def _sb_frames(self):
        """Storyboard 逐镜真实帧：低分辨率全段一次烘焙（fps=1）→ 按
        场景中点抽帧。缓存 = <thumbs>/sb/（契约页 mtime 失效）。

        html_bake 未构建时返回 200 + 空帧（对齐 /api/waveform 的"没有
        音轨返回 ok:true 且 peaks:null"先例——这是"本环境渲染不出帧"，
        不是错误；前端分镜卡有色块+文案兜底。返回 400 会让每次打开分镜
        都刷一屏 console 红字。"""
        import xmedia
        bake = xmedia.bake_bin()
        if not bake:
            return {"ok": True, "frames": [], "note":
                    "html_bake 未构建——分镜真实帧不可用，先用色块兜底"
                    "（cmake --build build --target html_bake）"}
        cache = os.path.join(self._thumbs_dir(), "sb")
        os.makedirs(cache, exist_ok=True)
        marker = os.path.join(cache, "done.json")
        html_mtime = os.path.getmtime(self.contract_path)
        if os.path.exists(marker):
            try:
                data = json.load(open(marker, encoding="utf-8"))
                if data.get("html_mtime") == html_mtime:
                    return data
            except (OSError, json.JSONDecodeError, ValueError):
                pass
        spec = self.current_spec or {}
        w, h = self._dims(spec)
        scale = 480.0 / max(1, max(w, h))
        bw = max(2, int(w * scale) // 2 * 2)
        bh = max(2, int(h * scale) // 2 * 2)
        total = parse_total(self.current_html, parse_scenes(self.current_html))
        sb_mp4 = os.path.join(cache, "sb.mp4")
        url = "file://" + urllib.parse.quote(self.contract_path)
        with _BAKE_LOCK:
            proc = subprocess.run(
                [bake, "--url", url, "--out", sb_mp4,
                 "--seconds", f"{total:.2f}", "--fps", "1",
                 "--width", str(bw), "--height", str(bh), "--clock", "seek", "--paint-wait", "1500"],
                capture_output=True, text=True, timeout=900)
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or proc.stdout or "")[-300:])
        urls = []
        for i, meta in enumerate(parse_scenes(self.current_html)):
            mid = (meta.get("start") or 0) + (meta.get("dur") or 3) / 2
            jpg = os.path.join(cache, f"s{i}.jpg")
            p2 = subprocess.run(
                ["ffmpeg", "-y", "-ss", f"{mid:.2f}", "-i", sb_mp4,
                 "-frames:v", "1", jpg], capture_output=True, timeout=60)
            urls.append(f"/api/sb-frame/{i}" if p2.returncode == 0 else None)
        data = {"html_mtime": html_mtime, "urls": urls, "w": bw, "h": bh}
        with open(marker, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False)
        return data

    # 每个 Arc 的示例 brief（真帧 tile 的内容——占位文案按它填充，
    # 展示的是「这个类型做出来长什么样」）
    ARC_PREVIEW_MESSAGE = {
        "launch": "给团队的新工作流发布片「一次搞定」",
        "problem-solution": "切片发布从半天缩短到一分钟",
        "listicle": "三个理由换掉手工剪辑",
        "explainer": "我们的视频管线是怎么工作的",
        "comparison": "手工剪辑对比确定性合成",
    }

    def _ensure_arc_previews(self):
        """Arc 示例真帧（Home「做个东西」宫格的真实画面墙，对齐 HF
        first-cut 的 survey tiles）。两级来源：
        1) 复用现有项目已验证的分镜帧（thumbs/sb/s*.jpg——分镜管线
           长期稳定，零烘焙成本）；
        2) 缺失的 Arc 用示例 spec 现烘（全局 bake 锁串行——并发
           html_bake 会互相拖死，实测整片纯黑）。
        单 Arc 失败静默（前端色条兜底）。"""
        import hf_storyboard
        import hf_compose
        import xmedia as _xm
        out_dir = os.path.expanduser("~/.xmedia/arc-previews")
        os.makedirs(out_dir, exist_ok=True)
        # 1) 现有项目分镜帧直接复用（按项目的 Arc 归属）
        try:
            projects = self._projects_list()
        except Exception:
            projects = []
        for p in projects:
            arc = p.get("arc") or ""
            jpg = os.path.join(out_dir, arc + ".jpg")
            if not arc or os.path.exists(jpg) or \
                    arc not in hf_storyboard.ARCS:
                continue
            d = os.path.join(self._projects_root(), p["name"])
            for cand in ("thumbs/sb/s1.jpg", "thumbs/sb/s0.jpg"):
                src = os.path.join(d, cand)
                if os.path.isfile(src) and os.path.getsize(src) > 3000:
                    try:
                        shutil.copyfile(src, jpg)
                        break
                    except OSError:
                        pass
        # 2) 缺失的 Arc 现烘（串行）
        bake = _xm.bake_bin()
        if not bake:
            return
        for arc in hf_storyboard.ARCS:
            jpg = os.path.join(out_dir, arc + ".jpg")
            if os.path.exists(jpg):
                continue
            try:
                with _BAKE_LOCK:
                    self._bake_arc_preview(bake, arc, out_dir)
            except Exception:
                continue  # 预览帧是增强不是依赖，失败保持色条兜底

    def _bake_arc_preview(self, bake, arc, out_dir):
        """单 Arc 示例 spec → compose → bake → 抽帧（调用方持锁）。"""
        import hf_storyboard
        import hf_compose
        d = os.path.join(out_dir, "work", arc)
        os.makedirs(d, exist_ok=True)
        spec = hf_storyboard.build(
            arc, self.ARC_PREVIEW_MESSAGE.get(arc, "示例视频"))
        with open(os.path.join(d, "spec.json"), "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False, indent=1)
        html_path = os.path.join(d, "contract.html")
        html = hf_compose.compose(spec)
        with open(html_path, "w", encoding="utf-8") as f:
            f.write(html)
        w, h = self._dims(spec)
        scale = 480.0 / max(1, max(w, h))
        bw = max(2, int(w * scale) // 2 * 2)
        bh = max(2, int(h * scale) // 2 * 2)
        scenes = parse_scenes(html)
        total = parse_total(html, scenes)
        if not scenes or total <= 0:
            return
        mp4 = os.path.join(d, "sb.mp4")
        proc = subprocess.run(
            [bake, "--url", "file://" + urllib.parse.quote(html_path),
             "--out", mp4, "--seconds", f"{total:.2f}", "--fps", "1",
             "--width", str(bw), "--height", str(bh),
             "--clock", "seek", "--paint-wait", "1500"],
            capture_output=True, text=True, timeout=900)
        if proc.returncode != 0:
            return
        jpg = os.path.join(out_dir, arc + ".jpg")
        # 全片逐帧采样 + 字节选优：页面加载延迟让内容整体后移且幅度
        # 不定（实测 2.5-3.5s），timeline 播完后契约页清场——固定候选
        # 点不可靠，直接每 0.5s 采一遍（fps=1 的 mp4 帧少，12 帧 ≈ 4s），
        # 取字节最大的帧（内容帧 3-11KB vs 空态 ~1-2KB）。跳过头部
        # 3s（GSAP 未就绪的纯背景区）。
        probe = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", mp4], capture_output=True, text=True,
            timeout=60)
        try:
            dur = float(probe.stdout.strip())
        except (ValueError, OSError):
            dur = total
        best = None
        k = 3.0
        while k < dur - 0.3:
            # 每个候选点独立文件名——共用一个路径会让 os.replace 拿到
            # 最后一次 ffmpeg 的输出（末尾清场空帧）而非字节最大的帧
            # （此前"间歇出纯色 tile"的真凶）。
            cand = os.path.join(d, f"pick-{int(k * 10):03d}.jpg")
            p3 = subprocess.run(
                ["ffmpeg", "-y", "-ss", f"{k:.2f}", "-i", mp4,
                 "-frames:v", "1", cand], capture_output=True, timeout=60)
            k += 0.5
            if p3.returncode != 0 or not os.path.exists(cand):
                continue
            sz = os.path.getsize(cand)
            if best is None or sz > best[0]:
                best = (sz, cand)
        if best and best[0] > 3000:
            os.replace(best[1], jpg)
        for f in os.listdir(d):
            if f.startswith("pick-"):
                try:
                    os.remove(os.path.join(d, f))
                except OSError:
                    pass

    def _projects_root(self):
        """Home 模式的项目根：每个项目 = <root>/<名>/（spec.json +
        contract.html + chat.jsonl…）。"""
        return os.path.expanduser(os.environ.get(
            "XMEDIA_STUDIO_PROJECTS", "~/.xmedia/projects"))

    def _projects_list(self):
        root = self._projects_root()
        out = []
        if not os.path.isdir(root):
            return out
        for name in sorted(os.listdir(root)):
            spec_file = os.path.join(root, name, "spec.json")
            if not os.path.isfile(spec_file):
                continue
            try:
                spec = json.load(open(spec_file, encoding="utf-8"))
            except (OSError, json.JSONDecodeError, ValueError):
                continue
            sb = spec.get("storyboard") or {}
            out.append({"name": name,
                        "arc": sb.get("arc", ""),
                        "message": (sb.get("message")
                                    or (spec.get("scenes") or [{}])[0]
                                    .get("text", ""))[:60],
                        "scenes": len(spec.get("scenes") or []),
                        "updated": time.strftime(
                            "%Y-%m-%d %H:%M",
                            time.localtime(os.path.getmtime(spec_file)))})
        return out

    def _project_rename(self, body):
        """项目重命名（HF 顶栏的铅笔行内编辑）：目录改名 + 返回新 url。
        服务实例状态由前端跳转新 url 时的 open_project 重建，无需原地
        重绑——但当前打开的就是该项目时，先解绑避免父页残留引用。"""
        import shutil
        name = str(body.get("name") or "").strip()
        new_name = str(body.get("new_name") or "").strip()
        if not re.fullmatch(r"[\w\-]+", new_name or ""):
            raise RuntimeError("项目名只能是字母/数字/下划线/连字符")
        if name == new_name:
            return {"ok": True, "name": new_name,
                    "url": "/project/" + new_name}
        root = self._projects_root()
        d_old = os.path.join(root, name)
        d_new = os.path.join(root, new_name)
        if not os.path.isdir(d_old):
            raise RuntimeError(f"项目 {name} 不存在")
        if os.path.isdir(d_new):
            raise RuntimeError(f"项目 {new_name} 已存在")
        os.rename(d_old, d_new)
        # 打开中的项目被改名：解绑契约/spec 引用（前端随即跳新地址）。
        if self.spec_path and \
                os.path.dirname(os.path.abspath(self.spec_path)) == \
                os.path.abspath(d_old):
            self.contract_path = os.path.join(d_new, "contract.html")
            self.spec_path = os.path.join(d_new, "spec.json")
            self.spec_dir = d_new
        return {"ok": True, "name": new_name,
                "url": "/project/" + new_name}

    def _project_delete(self, body):
        """项目删除（HF 管理能力）：整目录 rmtree。当前打开的项目
        被删时解绑引用（前端随即跳 Home）。"""
        import shutil
        name = str(body.get("name") or "").strip()
        if not re.fullmatch(r"[\w\-]+", name or ""):
            raise RuntimeError("项目名不合法")
        d = os.path.realpath(os.path.join(self._projects_root(), name))
        root = os.path.realpath(self._projects_root())
        if not d.startswith(root + os.sep) or not os.path.isdir(d):
            raise RuntimeError(f"项目 {name} 不存在")
        if self.spec_path and os.path.dirname(
                os.path.abspath(self.spec_path)) == d:
            # 打开中的项目：解绑（前端跳 Home 重建上下文）
            self.contract_path = None
            self.spec_path = None
            self.spec_dir = None
            self.current_spec = {}
            self.current_html = ""
        shutil.rmtree(d)
        return {"ok": True, "name": name}

    def _brand_delete(self, body):
        """品牌 kit 删除：JSON + 专属 logo/截图文件；default pin 指向
        该 kit 时一并清除。"""
        import shutil
        name = str(body.get("name") or "").strip()
        if not re.fullmatch(r"[\w\-]+", name or ""):
            raise RuntimeError("品牌名不合法")
        d = os.path.expanduser(os.environ.get(
            "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
        d = os.path.realpath(d)
        removed = []
        for rel in (name + ".json", "assets/" + name + "-logo.png",
                    "assets/" + name + "-logo.jpg",
                    "assets/" + name + "-logo.svg",
                    "assets/" + name + "-logo.webp",
                    "shots/" + name + "-home.jpg"):
            fp = os.path.realpath(os.path.join(d, rel))
            if fp.startswith(d + os.sep) and os.path.isfile(fp):
                os.remove(fp)
                removed.append(rel)
        pin = os.path.join(d, ".default")
        if os.path.isfile(pin) and open(pin, encoding="utf-8").read().strip() == name:
            os.remove(pin)
        return {"ok": True, "name": name, "removed": removed}

    def open_project(self, name):
        """Home → 编辑视图：重绑到项目目录的契约页/spec（单用户本地
        服务，直接重绑实例状态可接受）。"""
        d = os.path.join(self._projects_root(), name)
        html = os.path.join(d, "contract.html")
        spec = os.path.join(d, "spec.json")
        if not (re.fullmatch(r"[\w\-]+", name or "")
                and os.path.isfile(html) and os.path.isfile(spec)):
            raise RuntimeError(f"项目 {name} 不存在")
        self.contract_path = html
        self.spec_path = spec
        self.spec_dir = d
        with open(html, "r", encoding="utf-8") as f:
            self.current_html = f.read()
        self.current_spec = self._load_spec()
        self._register_spec_paths(self.current_spec)
        self._mix_key = None

    def _project_create(self, body):
        """一句话新建项目：Arc 叙事计划 → 骨架 spec → 契约页 → 可编辑。"""
        import hf_storyboard
        import hf_compose
        import hf_validate
        name = str(body.get("name") or "").strip()
        arc = str(body.get("arc") or "launch")
        message = str(body.get("message") or "").strip()
        if not name:
            # Home 表单承诺「项目名（可选，默认自动）」：自动名 = Arc +
            # 时间戳（正则合法 + 可读），冲突追加序号。
            name = f"{arc}-{time.strftime('%m%d-%H%M%S')}"
            root = self._projects_root()
            k = 2
            while os.path.isdir(os.path.join(root, name)):
                name = f"{name}-{k}"
                k += 1
        if not re.fullmatch(r"[\w\-]+", name):
            raise RuntimeError("项目名只能是字母/数字/下划线/连字符")
        if arc not in hf_storyboard.ARCS:
            raise RuntimeError(f"未知 Arc「{arc}」")
        if not message:
            raise RuntimeError("缺 message（一句话论点）")
        d = os.path.join(self._projects_root(), name)
        if os.path.isdir(d):
            raise RuntimeError(f"项目 {name} 已存在")
        os.makedirs(d, exist_ok=True)
        spec = hf_storyboard.build(
            arc, message, audience=str(body.get("audience") or ""),
            mood=str(body.get("mood") or ""),
            fmt=str(body.get("format") or "9:16"),
            fps=int(body.get("fps") or 30),
            voice=str(body.get("voice") or "Tingting"))
        spec["composition"]["id"] = name
        # 品牌 kit 注入（HF brand kits 语义）：Make Something 向导显式
        # 选 kit > workspace default pin > 无品牌保持 Arc 默认色板。
        brand_name = str(body.get("brand") or "").strip()
        if not brand_name:
            try:
                broot = os.path.expanduser(os.environ.get(
                    "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
                pin = os.path.join(broot, ".default")
                if os.path.isfile(pin):
                    brand_name = open(pin, encoding="utf-8").read().strip()
            except OSError:
                brand_name = ""
        if brand_name:
            try:
                broot = os.path.expanduser(os.environ.get(
                    "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
                bfile = os.path.join(broot, brand_name + ".json")
                if os.path.isfile(bfile):
                    brand = json.load(open(bfile, encoding="utf-8"))
                    sys.path.insert(0, TOOLS_DIR)
                    import hf_agent
                    spec, _ = hf_agent.apply_brand(spec, brand)
            except (OSError, json.JSONDecodeError, ValueError):
                pass
        spec_file = os.path.join(d, "spec.json")
        with open(spec_file, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False, indent=1)
            f.write("\n")
        # 手动绑定（open_project 要求契约页已生成，而这里正是要生成它）
        self.contract_path = os.path.join(d, "contract.html")
        self.spec_path = spec_file
        self.spec_dir = d
        self.current_spec = spec
        self._register_spec_paths(spec)
        self._compose()   # 写项目目录的 contract.html
        findings, errors, _, _ = hf_validate.validate(self.current_html)
        # 首剪的听觉维度（HF first-cut 产出"能看能听"的一条）：旁白
        # 后台生成——用户跳进编辑器时分镜已就绪，TTS 完成后刷新即见。
        # 线程失败静默（无 say/音色环境 TTS 失败是常态）。
        threading.Thread(target=self._safe_tts, daemon=True).start()
        return {"name": name, "url": "/project/" + name,
                "scenes": len(spec.get("scenes") or []),
                "errors": errors}

    def _safe_tts(self):
        try:
            self._run_tts()
            self._compose()
        except Exception:
            pass

    def _cancel_bake(self):
        """终止进行中的导出（Export 面板的取消渲染）。"""
        if not self.job or self.job["proc"].poll() is not None:
            raise RuntimeError("无进行中的导出任务")
        self.job["cancelled"] = True
        self.job["proc"].terminate()
        return {"cancelled": True}

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
                "cancelled": bool(self.job.get("cancelled")),
                "out": self.job["out"], "elapsed": time.time() - self.job["start"],
                "tail": self.job["tail"]}

    # ---- Agent / ASR / 素材导入（对位 Remotion「Make videos agentically」）----
    def _spec_root(self):
        return os.path.dirname(self.spec_path) if self.spec_path else self.spec_dir

    def _apply_spec(self, spec):
        """外部（Agent / import / LLM）产出的 spec 统一走校验 → 写回 → 重组。"""
        import hf_agent
        hf_agent.validate_spec(spec, self._spec_root())
        self._record_run()
        self._write_spec(spec)
        return self._compose()

    PROJECT_RUNS_MAX = 20

    def _project_path(self):
        return os.path.join(self._spec_root(), "project.json")

    def _load_runs(self):
        try:
            with open(self._project_path(), encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict) and isinstance(data.get("runs"), list):
                return data["runs"]
        except (OSError, json.JSONDecodeError, ValueError):
            pass
        return []

    def _save_runs(self, runs):
        data = {"name": os.path.splitext(os.path.basename(
                    self.spec_path or "project"))[0],
                "updated": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "runs": runs}
        try:
            with open(self._project_path(), "w", encoding="utf-8") as f:
                json.dump(data, f, ensure_ascii=False)
        except OSError:
            pass

    def _record_run(self, message="", backend="rule"):
        """spec 变更前记录快照（run 栈）：{ts, message, backend,
        spec_before}。持久化到 <spec 根>/project.json，上限 20 轮。"""
        runs = self._load_runs()
        runs.append({"ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                     "message": str(message)[:200],
                     "backend": backend,
                     "spec_before": self.current_spec})
        del runs[:-self.PROJECT_RUNS_MAX]
        self._save_runs(runs)

    def _undo_run(self, idx):
        """撤销第 idx 轮（0-based）：恢复该轮之前的状态，丢弃该轮及之后
        的所有轮（时间线一致性）。"""
        runs = self._load_runs()
        if not (0 <= idx < len(runs)):
            raise RuntimeError(f"run {idx} 不存在（共 {len(runs)} 轮）")
        run = runs[idx]
        spec_before = run.get("spec_before")
        if not isinstance(spec_before, dict) or not spec_before.get("scenes"):
            raise RuntimeError(f"run {idx} 没有可恢复的快照")
        runs = runs[:idx]
        self._save_runs(runs)
        return self._apply_spec(spec_before)

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
        # 旁白字幕依赖 narration——重组契约页把字幕 DOM 带进去
        # （不重组的话预览/导出里字幕永远不出现，用户观感"TTS 没用"）。
        self._compose()
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

    def _brands_dir(self):
        return os.path.expanduser(
            os.environ.get("XMEDIA_BRAND_DIR", "~/.xmedia/brands"))

    def _brands_list(self):
        """品牌 kits 清单（HF Brand kits 左栏列表）：名/来源/色板/字体/
        logo/是否默认。brand JSON 很小，全量随列表下发，详情免二次请求。"""
        d = self._brands_dir()
        pin = os.path.join(d, ".default")
        pinned = ""
        if os.path.isfile(pin):
            try:
                pinned = open(pin, encoding="utf-8").read().strip()
            except OSError:
                pinned = ""
        kits = []
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if not f.endswith(".json"):
                    continue
                try:
                    b = json.load(open(os.path.join(d, f),
                                       encoding="utf-8"))
                except (OSError, json.JSONDecodeError):
                    continue
                b["_default"] = (f[:-5] == pinned)
                # Files 清单（HF kit 的 Files 分类）：该 kit 专属的
                # 落盘文件（JSON/logo/截图），相对路径+大小。
                base_name = f[:-5]
                files = []
                for rel in (f, "assets/" + base_name + "-logo.png",
                            "assets/" + base_name + "-logo.jpg",
                            "assets/" + base_name + "-logo.svg",
                            "assets/" + base_name + "-logo.webp",
                            "shots/" + base_name + "-home.jpg"):
                    fp = os.path.join(d, rel)
                    if os.path.isfile(fp):
                        files.append({"path": rel,
                                      "bytes": os.path.getsize(fp)})
                b["_files"] = files
                kits.append(b)
        return {"brands": kits}

    def _brand_scan_api(self, body):
        """HF Brand kits 的「Scan my site」：抓站点色板/字体/logo 存 kit
        （subprocess 复用 xmedia brand scan，扫描同步、至多 ~60s）。"""
        url = str(body.get("url") or "").strip()
        if not url:
            raise ValueError("需要 url")
        name = re.sub(r"[^\w\-]", "", str(body.get("name") or ""))[:24]
        if not name:
            host = url.split("//", 1)[-1].split("/", 1)[0]
            host = host.split(":")[0].lower()
            if host.startswith("www."):
                host = host[4:]
            name = re.sub(r"[^\w\-]", "", host.split(".")[0]) or "scanned"
        try:
            r = subprocess.run(
                [sys.executable, os.path.join(TOOLS_DIR, "xmedia.py"),
                 "brand", "scan", name, "--url", url],
                capture_output=True, text=True, timeout=60,
                cwd=os.path.dirname(TOOLS_DIR))
        except subprocess.TimeoutExpired:
            raise ValueError("扫描超时（网站响应太慢），稍后再试")
        fpath = os.path.join(self._brands_dir(), name + ".json")
        if r.returncode != 0 or not os.path.isfile(fpath):
            tail = [ln for ln in (r.stderr or r.stdout or "").strip()
                    .splitlines() if ln.strip()]
            raise ValueError("扫描失败：" +
                             (tail[-1] if tail else "未知错误"))
        brand = json.load(open(fpath, encoding="utf-8"))
        return {"ok": True, "name": name, "brand": brand}

    def _brand_default(self, body):
        """default pin（HF brand kits 的 default 语义）：新项目创建时
        自动应用该品牌；name 为空 = 取消 pin。"""
        name = str(body.get("name") or "").strip()
        d = self._brands_dir()
        os.makedirs(d, exist_ok=True)
        pin = os.path.join(d, ".default")
        if not name:
            if os.path.isfile(pin):
                os.remove(pin)
            return {"ok": True, "default": ""}
        if not os.path.isfile(os.path.join(d, name + ".json")):
            raise ValueError(f"品牌 {name} 不存在")
        with open(pin, "w", encoding="utf-8") as fh:
            fh.write(name + "\n")
        return {"ok": True, "default": name}

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
            elif action == "reference":
                out["reference"] = self._run_reference(args)
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

    AGENT_CHAT_TIMEOUT_S = 900
    AGENT_HISTORY_ROUNDS = 5

    @staticmethod
    def _agent_probe():
        """本地 agent CLI 探测：
        1. XMEDIA_AGENT_CMD 环境变量（最高优先）——任意 agent CLI 接入：
           JSON 数组（'["codebuddy","-p"]'）或 shell 串（'codebuddy -p'）。
           约定：CLI 收到【prompt 作为最后一个参数】，输出纯文本回复
           （claude 的 --output-format json 解析仅对 cli=="claude" 生效）。
           XMEDIA_AGENT_NAME 可选显示名（默认取可执行文件名）。
        2. 内置探测：claude/codebuddy -p --output-format stream-json
           （逐行 JSON 事件流——assistant text 实时进 tail，进程退出 =
           真完成）→ codex exec。"""
        custom = os.environ.get("XMEDIA_AGENT_CMD")
        if custom:
            cmd = None
            try:
                parsed = json.loads(custom)
                if isinstance(parsed, list):
                    cmd = [str(x) for x in parsed]
            except (json.JSONDecodeError, ValueError):
                cmd = None
            if cmd is None:
                cmd = shlex.split(custom)
            if cmd:
                name = (os.environ.get("XMEDIA_AGENT_NAME")
                        or os.path.basename(cmd[0]))
                return {"cli": name, "cmd": cmd, "custom": True}
        for name, cmd in (("claude", ["claude", "-p", "--output-format",
                                      "stream-json", "--verbose"]),
                          ("codebuddy", ["codebuddy", "-p",
                                         "--output-format", "stream-json",
                                         "--permission-mode",
                                         "acceptEdits"]),
                          ("codex", ["codex", "exec"])):
            path = shutil.which(name)
            if path:
                return {"cli": name, "cmd": cmd, "path": path}
        return {"cli": None}

    def _chat_history_tail(self):
        path = os.path.join(self._spec_root(), "chat.jsonl")
        rounds = []
        try:
            with open(path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            rounds.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
        except OSError:
            pass
        return rounds[-self.AGENT_HISTORY_ROUNDS:]

    def _chat_log(self, message, reply, backend):
        """对话历史持久化：<spec 根>/chat.jsonl 逐行追加（失败不阻塞）。"""
        path = os.path.join(self._spec_root(), "chat.jsonl")
        try:
            with open(path, "a", encoding="utf-8") as f:
                f.write(json.dumps(
                    {"ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
                     "backend": backend, "user": message,
                     "assistant": reply}, ensure_ascii=False) + "\n")
        except OSError:
            pass

    def _agent_chat_body(self, message, probe):
        """agent prompt 构造（路径/skill 工作流/叙事纪律/历史）。"""
        spec_path = self.spec_path or ""
        root = self._spec_root()
        history = self._chat_history_tail()
        hist_txt = "\n".join(
            f"[{r.get('backend', '?')}] 用户: {str(r.get('user', ''))[:120]}"
            f"\n助手: {str(r.get('assistant', ''))[:200]}"
            for r in history) or "（无）"
        # 品牌规则注入（HF brand kit 的 Do's and Don'ts prose）：默认
        # 品牌存在时读其 rules 注入 agent 上下文。
        brand_rules = ""
        try:
            broot = os.path.expanduser(os.environ.get(
                "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
            pin = os.path.join(broot, ".default")
            if os.path.isfile(pin):
                bname = open(pin, encoding="utf-8").read().strip()
                bfile = os.path.join(broot, bname + ".json")
                if os.path.isfile(bfile):
                    brand = json.load(open(bfile, encoding="utf-8"))
                    rules = brand.get("rules") or {}
                    dos = rules.get("dos") or []
                    donts = rules.get("donts") or []
                    if dos or donts:
                        brand_rules = (
                            "\n【品牌规则（brand kit，必须遵守）】\n"
                            + ("".join(f"- 应该：{d}\n" for d in dos))
                            + ("".join(f"- 禁止：{d}\n" for d in donts)))
        except (OSError, json.JSONDecodeError, ValueError):
            pass
        # 叙事纪律注入：spec.storyboard.direction 存在时（HF Storyboard
        # 工作流的方向块），agent 逐帧编辑必须遵守这些规则。
        sb = (self.current_spec or {}).get("storyboard") or {}
        discipline = ""
        direction = sb.get("direction") or {}
        if direction:
            two = direction.get("two_color") or {}
            discipline = (
                "\n【叙事纪律（storyboard 方向块，逐帧编辑必须遵守）】\n"
                f"- Arc: {sb.get('arc', '')} · message: "
                f"{sb.get('message', '')[:80]}\n"
                + (f"- 两色纪律: ground {two.get('ground', '')} / ink "
                   f"{two.get('ink', '')}——强调只允许反转/放大/密度，"
                   "永不引入第三色\n" if two else "")
                + (f"- breather: 第 {direction.get('breather', '?') + 1} 帧"
                   "是刻意更静的节拍，不要过度动画\n"
                   if "breather" in direction else "")
                + (f"- 负面清单（禁止）: "
                   f"{('、'.join(direction.get('banned', [])))}\n"
                   if direction.get("banned") else ""))
        # HF chatSystemPrompt 的编排规则移植（chatRules.mjs 对齐）：
        # plan/edit 分流 / 文件白名单 / 历史感知 / gate 循环 / 时长联动 /
        # 回复风格——语义照搬，命令换 xmedia 对应物。
        runs = self._load_runs()
        hist_note = ""
        for r in reversed(runs):
            if r.get("backend") in ("claude", "codebuddy", "agent"):
                hist_note = (f"- 历史感知：项目在 {r['ts']} 之后有人（非 "
                             "agent）改过的话，在那些改动之上继续，永不撤销"
                             "人的修改\n")
                break
        return (
            "你在 xmedia-kit Studio 的 agent 模式里，与用户一起编辑这个"
            "文件夹里的视频项目。像终端里的编码 agent 一样读用户消息。\n"
            "【消息分流】要求计划/大纲/头脑风暴/解释、或提出问题的消息——"
            "只用文字回答（被要求时给一份平实的计划），不改任何文件、不跑"
            "任何会写文件的命令，可以提议继续执行。消息要求做或改什么、或"
            "在计划后说继续——才动手改文件。\n"
            "【文件白名单】只编辑 spec.json 与 contract.html（以及你自己"
            "创建的临时文件）；chat.jsonl / project.json / __thumbs__/ 是"
            "应用自己的，读可以、永不删除或改动。\n"
            "【历史感知】改一个已存在的项目前，看 project.json 的 runs："
            "你上一次编辑之后由人或其他 agent 做的改动，要在那些改动之上"
            "继续，永不撤销它们。\n"
            f"【gate 循环】每次改动 spec 后：python3 {TOOLS_DIR}/"
            "hf_validate.py "
            f"{spec_path} 修到 0 errors → 用分镜真实帧（/api/sb-frames 或 "
            "xmedia.py render 抽帧）看被改的瞬间，回复前自己看帧——"
            "「看起来顺」不是验证，用数字与帧说话。\n"
            "【时长联动】视频总长 = 所有场景 duration 之和（compose 自动"
            "排 data-start）：新增/删除/加长/缩短场景时，确认后续场景"
            "时长与总长衔接，视频永不留空白尾巴、永不切掉一镜。\n"
            "【回复风格】一两句平实的话，说屏幕上改了什么。名字用它们"
            "显示的样子（「第一镜的大标题」），不用 id/文件名/mount 这类"
            "词，除非用户先用了。从不声称没验证的东西。\n"
            f"【路径】spec: {spec_path} · 目录: {root} · 仓库: {ROOT_DIR}"
            f"\n- schema 文档: {ROOT_DIR}/docs/HF_COMPOSE.md（改 spec 前"
            "先读它的 schema 段）\n"
            f"- 【skill 工作流】大改动（新项目/多场景结构）先读 "
            f"{ROOT_DIR}/skills/launch/SKILL.md 按其 Order 执行；技能"
            "清单里没有的技能，看 skills/ 目录下有没有对应的 SKILL.md\n"
            f"{brand_rules}{discipline}\n"
            f"最近对话（供参考）：\n{hist_txt}\n\n用户请求：{message}\n")

    def _spec_diff_summary(self, before_scenes, after_scenes):
        """agent 前后场景 diff 摘要（改动明细卡）。"""
        out = []
        if len(before_scenes) != len(after_scenes):
            out.append(f"场景数 {len(before_scenes)} → {len(after_scenes)}")
        for i, (b, a) in enumerate(zip(before_scenes, after_scenes)):
            for k in ("text", "subtitle", "duration", "bg", "type"):
                bv, av = (b or {}).get(k), (a or {}).get(k)
                if bv != av:
                    sb_ = str(bv)[:24] if bv is not None else "—"
                    sa_ = str(av)[:24] if av is not None else "—"
                    out.append(f"镜{i+1}.{k}: {sb_} → {sa_}")
        return out[:10]

    def _start_agent_chat(self, message):
        """agent chat job 化：后台线程驱动 CLI，前端轮询 tail——
        HF 的 agent 过程可视化（"正在改第 2 镜…"实时可见）。"""
        probe = self._agent_probe()
        if not probe["cli"]:
            return {"job": None, "agent": "none", "mode": "help",
                    "reply": "本机未检测到可用的 agent CLI（claude / codex）。\n"
                             "接入方式二选一：\n"
                             "1. 安装 Claude Code / CodeBuddy 后重启 Studio\n"
                             "2. 外部 agent + skill 驱动：.agents/skills/"
                             "xmedia-ai-video/SKILL.md\n"
                             "3. 继续用「规则引擎」模式"}
        job_id = self.agent_chat_seq
        self.agent_chat_seq += 1
        spec_path = self.spec_path or ""
        before_scenes = json.loads(json.dumps(
            (self.current_spec or {}).get("scenes") or []))
        job = {"id": job_id, "message": message, "agent": probe["cli"],
               "running": True, "tail": "（启动 agent…）", "reply": "",
               "spec_updated": False, "error": None, "done": False,
               "diff": []}
        self.agent_jobs[job_id] = job
        th = threading.Thread(
            target=self._agent_chat_thread,
            args=(job, message, probe, spec_path, before_scenes),
            daemon=True)
        th.start()
        return {"job": job_id, "agent": probe["cli"]}

    def _agent_chat_thread(self, job, message, probe, spec_path,
                           before_scenes):
        cli = probe["cli"]
        root = self._spec_root()
        prompt = self._agent_chat_body(message, probe)
        spec_mtime_before = (os.path.getmtime(spec_path)
                             if spec_path and os.path.exists(spec_path)
                             else 0)
        self._record_run(message, cli)   # agent 改文件前压快照
        try:
            proc = subprocess.Popen(
                probe["cmd"] + [prompt], stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, cwd=root or ".")
        except OSError as e:
            job.update({"running": False, "done": True,
                        "reply": f"agent 启动失败：{e}", "error": "spawn"})
            self._chat_log(message, job["reply"], cli)
            return
        tail_lines = []
        texts = []          # assistant text 块累积（= 回复）
        tool_steps = []     # 工具步骤（tail 显示）
        is_error = False
        import selectors
        sel = selectors.DefaultSelector()
        sel.register(proc.stdout, selectors.EVENT_READ)
        deadline = time.time() + self.AGENT_CHAT_TIMEOUT_S
        buf = ""
        idle = 0   # 无新输出的轮询计数（0.5s/轮，挂起兜底用）
        while True:
            if time.time() > deadline:
                proc.kill()
                job.update({"running": False, "done": True,
                            "reply": f"agent 运行超时"
                                     f"（{self.AGENT_CHAT_TIMEOUT_S}s）",
                            "error": "timeout"})
                self._chat_log(message, "（agent 运行超时）", cli)
                return
            events = sel.select(timeout=0.5)
            eof = False
            for key, _ in events:
                line = key.fileobj.readline()
                if line == "":
                    eof = True   # stdout 关闭 = agent 输出完成
                    break
                line = line.strip()
                if not line:
                    continue
                # stream-json 事件流（claude/codebuddy 同构）：逐行 JSON，
                # assistant 的 text 块实时进 tail（HF 过程可视化）、
                # tool_use 显示工具步骤、result 是最终回复。
                obj = None
                try:
                    obj = json.loads(line)
                except (json.JSONDecodeError, ValueError):
                    tail_lines.append(line[:120])
                    job["tail"] = "\n".join(tail_lines[-6:])
                    continue
                etype = obj.get("type")
                if etype == "assistant":
                    msg = obj.get("message") or {}
                    for c in (msg.get("content") or []):
                        ctype = c.get("type")
                        if ctype == "text" and c.get("text"):
                            texts.append(c["text"])
                            job["tail"] = c["text"][:300]
                        elif ctype == "tool_use":
                            step = "[工具] " + c.get("name", "?")
                            tool_steps.append(step)
                            job["tail"] = step
                elif etype == "result":
                    texts.append(str(obj.get("result") or ""))
                    is_error = bool(obj.get("is_error"))
                    parsed = True
            if eof or proc.poll() is not None:
                break
            if not events:
                idle += 1
            # 挂起兜底：stdout 长时间无新行且进程未退（agent 完成后偶发
            # 挂住，实测）——40s 无新行视为完成，收 tail。
            if buf and idle >= 80:
                proc.kill()
                break
        proc.wait()
        # stream-json 收集的 texts 最后一条 = agent 的最终回复；
        # is_error 已在事件流里提取。
        reply = (texts[-1] if texts else "").strip() or \
            ("\n".join(tool_steps[-3:]) or "（agent 无文本输出）")
        spec_changed = (bool(spec_path) and os.path.exists(spec_path) and
                        os.path.getmtime(spec_path) > spec_mtime_before)
        job["spec_updated"] = spec_changed
        if proc.returncode != 0 and not parsed:
            job.update({"running": False, "done": True,
                        "reply": (proc.stderr or reply_raw)[-600:]
                                  or "agent 非零退出",
                        "error": f"exit {proc.returncode}"})
            self._chat_log(message, job["reply"], cli)
            return
        if is_error:
            job["error"] = ("agent 上报错误（常见为 CLI 配置的模型不可用："
                            "claude --model 换可用模型）")
        job["reply"] = reply[-1500:] or "（agent 无输出）"
        job["done"] = True
        # spec 被改 → 重载重组 + diff 摘要（线程内完成，前端轮询到 done）
        if spec_changed:
            self.current_spec = self._load_spec()
            self._compose()
            job["diff"] = self._spec_diff_summary(before_scenes,
                                                  self.current_spec.get(
                                                      "scenes") or [])
        self._chat_log(message, job["reply"], cli)

    def _agent_chat_status(self, job_id):
        job = self.agent_jobs.get(job_id)
        if job is None:
            raise RuntimeError("无此 agent 任务")
        return {"running": job.get("running", False),
                "done": job.get("done", False),
                "agent": job.get("agent"),
                "tail": job.get("tail", ""),
                "reply": job.get("reply", ""),
                "spec_updated": job.get("spec_updated", False),
                "diff": job.get("diff", []),
                "error": job.get("error")}


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

        def _do_get_impl(self):
            path = urllib.parse.urlparse(self.path).path
            srv = server_ref
            if path == "/":
                # Home 模式（--home 启动）= 项目管理页（稳定启动器）；
                # 直接指定契约页启动 = 编辑视图（旧行为兼容）。
                if srv.home_mode or not srv.contract_path:
                    self._send(200, home_html(srv._projects_list()),
                               "text/html; charset=utf-8")
                else:
                    self._send(200, studio_ui(), "text/html; charset=utf-8")
            elif path.startswith("/project/"):
                name = urllib.parse.unquote(
                    path[len("/project/"):]).strip("/")
                try:
                    srv.open_project(name)
                except RuntimeError as e:
                    self._send(404, str(e), "text/plain")
                    return
                self._send(200, studio_ui(), "text/html; charset=utf-8")
            elif path == "/api/templates":
                import hf_compose
                registry = hf_compose.load_custom_templates(
                    srv._spec_root())
                self._json({"templates": [
                    {"name": d.get("name", ""),
                     "description": d.get("description", ""),
                     "slots": d.get("slots", [])}
                    for d in sorted(registry.values(),
                                    key=lambda x: x.get("name", ""))]})
            elif path == "/api/inspirations":
                # 灵感库（HF Inspirations 的本地对应）：内置示例 brief
                # 一键新建——冷启动不用从零想 brief。
                self._json({"inspirations": [
                    {"id": "launch-20s", "arc": "launch",
                     "title": "20 秒产品发布片",
                     "hint": "Hook → 亮点 → 证明 → CTA 的标准四段",
                     "message": "给团队发布我们的新工作流「一次搞定」，"
                                "把发布节奏压缩到 20 秒"},
                    {"id": "explainer-101", "arc": "explainer",
                     "title": "一分钟讲清一件事",
                     "hint": "What → Why → How 的科普节奏",
                     "message": "讲清楚我们的确定性视频管线是怎么工作的"},
                    {"id": "listicle-3", "arc": "listicle",
                     "title": "三个理由清单体",
                     "hint": "numbered enumeration，条目逐一点亮",
                     "message": "换掉手工剪辑的三个理由"},
                    {"id": "compare-orbit", "arc": "comparison",
                     "title": "新旧方案对比",
                     "hint": "之前/之后 双列擦入，证明差异化",
                     "message": "手工剪辑对比确定性合成「一次合成永不翻车」"},
                    {"id": "problem-fix", "arc": "problem-solution",
                     "title": "痛点-方案钩子",
                     "hint": "Hook 抛问题 → 方案反转 → 证明",
                     "message": "切片发布还在被手工流程拖慢「从半天到一分钟」"},
                ]})
            elif path == "/api/brands":
                self._json(srv._brands_list())
            elif path.startswith("/api/brand-shot/"):
                # 品牌网页截图（scan 时 html_bake 烘的首屏视觉证据）
                nm = path[len("/api/brand-shot/"):]
                if not re.fullmatch(r"[\w\-]+", nm or ""):
                    self._send(404, "not found", "text/plain")
                    return
                broot = os.path.expanduser(os.environ.get(
                    "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
                jpg = os.path.join(broot, "shots", nm + "-home.jpg")
                if not os.path.isfile(jpg):
                    self._send(404, "not found", "text/plain")
                    return
                with open(jpg, "rb") as fh:
                    self._send(200, fh.read(), "image/jpeg")
            elif path.startswith("/api/brand-logo/"):
                self._serve_brand_logo(path[len("/api/brand-logo/"):])
            elif path.startswith("/api/project-thumb/"):
                # Home 项目卡缩略图（HF Home 卡片的项目截图同款）：
                # 分镜帧产物（__thumbs__/sb/）直读，无则 404（前端
                # 色块兜底）。s1 优先（首镜中点可能抽到入场半透明态）。
                nm = path[len("/api/project-thumb/"):]
                if not re.fullmatch(r"[\w\-]+", nm or ""):
                    self._send(404, "not found", "text/plain")
                    return
                for f in ("s1.jpg", "s0.jpg"):
                    jpg = os.path.join(srv._projects_root(), nm,
                                       "__thumbs__", "sb", f)
                    if os.path.isfile(jpg):
                        with open(jpg, "rb") as fh:
                            self._send(200, fh.read(), "image/jpeg")
                        return
                self._send(404, "not found", "text/plain")
            elif path.startswith("/api/arc-preview/"):
                arc = path[len("/api/arc-preview/"):]
                if not re.fullmatch(r"[\w\-]+", arc or ""):
                    self._send(404, "not found", "text/plain")
                    return
                jpg = os.path.expanduser(
                    f"~/.xmedia/arc-previews/{arc}.jpg")
                if not os.path.isfile(jpg):
                    self._send(404, "not found", "text/plain")
                    return
                with open(jpg, "rb") as f:
                    self._send(200, f.read(), "image/jpeg")
            elif path == "/api/agent/chat/status":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(
                    self.path).query)
                self._json(srv._agent_chat_status(
                    int((q.get("id") or ["0"])[0])))
            elif path == "/api/projects":
                self._json({"projects": srv._projects_list()})
            elif path == "/api/state":
                import hf_validate
                findings, errors, _, _ = hf_validate.validate(
                    srv.current_html, spec=srv.current_spec)
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
                                "XMEDIA_LLM_ENDPOINT")), "mcp": True,
                                "cli": srv._agent_probe()["cli"]},
                            "ref_video": (EXTERNAL_ASSET_PREFIX +
                                          srv.ref_video)
                                          if srv.ref_video else None,
                            "runs": [{"idx": i, "ts": r.get("ts", ""),
                                      "message": r.get("message", ""),
                                      "backend": r.get("backend", "")}
                                     for i, r in enumerate(srv._load_runs())],
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
            elif path == "/api/job/current":
                # 进行中的导出任务（expOpen 恢复进度显示用）：
                # job 还在跑 → {job: id}；无/已结束 → {job: None}。
                j = srv.job
                if j and j.get("proc") and j["proc"].poll() is None:
                    self._json({"job": j["id"], "out": j.get("out", "")})
                else:
                    self._json({"job": None})
            elif path == "/api/job":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
                try:
                    self._json(srv._job_status(int(q["id"][0])))
                except (RuntimeError, KeyError, ValueError) as e:
                    self._json({"error": str(e)}, 400)
            elif path == "/api/sb-frames":
                self._json(srv._sb_frames())
            elif path.startswith("/api/sb-frame/"):
                self._serve_sb_frame(
                    path[len("/api/sb-frame/"):])
            elif path == "/api/thumbs":
                q = urllib.parse.parse_qs(urllib.parse.urlparse(
                    self.path).query)
                raw = (q.get("path") or [""])[0]
                try:
                    n = max(1, min(24, int((q.get("n") or ["8"])[0])))
                    w = max(16, min(480, int((q.get("w") or ["120"])[0])))
                    h = max(16, min(480, int((q.get("h") or ["68"])[0])))
                    s0 = max(0.0, float((q.get("s") or ["0"])[0] or 0))
                    d0 = max(0.0, float((q.get("d") or ["0"])[0] or 0))
                    frames = srv.build_thumbs(raw, n=n, w=w, h=h,
                                              start=s0, dur=d0)
                    self._json({"ok": True, "frames": frames,
                                "count": len(frames)})
                except Exception as e:
                    self._json({"ok": False, "error": str(e), "frames": []}, 400)
            elif path == "/api/waveform":
                # 真实音频峰值（时间轴波形）。没有音轨返回 ok:true 且
                # peaks:null——这是"这条素材没声音"，不是错误，前端据此
                # 回退到示意波形并标注。
                q = urllib.parse.parse_qs(urllib.parse.urlparse(
                    self.path).query)
                raw = (q.get("path") or [""])[0]
                try:
                    n = max(8, min(400, int((q.get("n") or ["48"])[0])))
                    s0 = max(0.0, float((q.get("s") or ["0"])[0] or 0))
                    d0 = max(0.0, float((q.get("d") or ["0"])[0] or 0))
                    peaks = srv.build_waveform(raw, n=n, start=s0, dur=d0)
                    self._json({"ok": True, "peaks": peaks,
                                "count": len(peaks) if peaks else 0})
                except Exception as e:
                    self._json({"ok": False, "error": str(e), "peaks": None},
                               400)
            elif path.startswith(THUMBS_PREFIX):
                self._serve_thumb(path)
            else:
                self._serve_asset(path)

        def do_GET(self):
            """异常兜底：GET handler 抛错（如 html_bake 缺失的 sb-frames）
            回 400 JSON，而不是杀连接让前端静默失败——与 do_POST 的错误
            语义一致。若响应头已发出（流式路由已开始写），二次兜底只记
            不抛（连接此时只能关闭）。"""
            try:
                self._do_get_impl()
            except (RuntimeError, ValueError, OSError, KeyError) as e:
                try:
                    self._json({"error": str(e)}, 400)
                except Exception:
                    pass

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
                elif path == "/api/reference/video":
                    self._json(srv._set_ref_video(
                        str(body.get("path") or "")))
                elif path == "/api/reference/video/clear":
                    srv.ref_video = None
                    self._json({"cleared": True})
                elif path == "/api/project/create":
                    self._json(srv._project_create(body))
                elif path == "/api/project/rename":
                    self._json(srv._project_rename(body))
                elif path == "/api/project/delete":
                    self._json(srv._project_delete(body))
                elif path == "/api/brand/delete":
                    self._json(srv._brand_delete(body))
                elif path == "/api/annotation/add":
                    if not isinstance(srv.current_spec, dict):
                        raise RuntimeError("未绑定 spec 文件")
                    # 手绘标注原子追加（Draw 笔划的持久化）：服务端
                    # append 消除前端 read-modify-write 竞态——连续
                    # 快速笔划不再互相覆盖丢笔划。
                    import hf_agent
                    strokes = body.get("strokes") or []
                    if not isinstance(strokes, list) or not strokes:
                        raise ValueError("strokes 必须是非空数组")
                    spec = srv.current_spec
                    spec["annotations"] = (spec.get("annotations") or []) + \
                        [s for s in strokes if isinstance(s, dict) and
                         isinstance(s.get("points"), list) and
                         len(s["points"]) >= 2]
                    hf_agent.validate_spec(spec, srv._spec_root())
                    srv._write_spec(spec)
                    srv._compose()
                    self._json({"ok": True,
                                "count": len(spec["annotations"])})
                elif path == "/api/annotation/clear":
                    if not isinstance(srv.current_spec, dict):
                        raise RuntimeError("未绑定 spec 文件")
                    import hf_agent
                    spec = srv.current_spec
                    spec["annotations"] = []
                    hf_agent.validate_spec(spec, srv._spec_root())
                    srv._write_spec(spec)
                    srv._compose()
                    self._json({"ok": True})
                elif path == "/api/reveal":
                    # 在 Finder 中定位导出产物（本地产品实用项）。
                    # realpath 前缀校验项目根（防 open -R 任意路径）。
                    import shutil
                    target = str(body.get("path") or "")
                    real = os.path.realpath(target)
                    root_real = os.path.realpath(srv._projects_root())
                    if not real.startswith(root_real + os.sep):
                        raise RuntimeError("路径不在项目目录内")
                    if not os.path.isfile(real):
                        raise RuntimeError("文件不存在")
                    subprocess.run(["open", "-R", real], timeout=15,
                                   check=False)
                    self._json({"ok": True})
                elif path == "/api/spec-save":
                    # 轻量 spec 保存（Comment/标注类元数据）：写回不重组
                    # ——评论不进契约页，重组是浪费。
                    import hf_validate
                    srv.current_spec = body["spec"]
                    srv._write_spec(srv.current_spec)
                    findings, errors, _, _ = hf_validate.validate(
                        srv.current_html, spec=srv.current_spec)
                    self._json({"ok": True, "errors": errors})
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
                    self._json({"job": srv._start_bake({
                        "quality": body.get("quality"),
                        "fmt": body.get("fmt"),
                        "fps": body.get("fps")})})
                elif path == "/api/bake/cancel":
                    self._json(srv._cancel_bake())
                elif path == "/api/project/undo":
                    self._json({"composed": srv._undo_run(
                        int(body.get("run", -1)))})
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
                elif path == "/api/brand/scan":
                    self._json(srv._brand_scan_api(body))
                elif path == "/api/brand/default":
                    self._json(srv._brand_default(body))
                elif path == "/api/agent":
                    self._json(srv._run_agent(str(body.get("message") or "")))
                elif path == "/api/agent/chat":
                    self._json(srv._start_agent_chat(
                        str(body.get("message") or "")))
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

        def _serve_sb_frame(self, idx):
            srv = server_ref
            idx_s = re.fullmatch(r"\d+", idx or "")
            if not idx_s:
                self._send(404, "not found", "text/plain")
                return
            jpg = os.path.join(srv._thumbs_dir(), "sb",
                                  f"s{idx}.jpg")
            if not os.path.isfile(jpg):
                self._send(404, "not found", "text/plain")
                return
            with open(jpg, "rb") as f:
                self._send(200, f.read(), "image/jpeg")

        def _serve_brand_logo(self, name):
            """品牌 logo 图片（详情页预览）。仅认 kit 名（防路径穿越），
            实际文件从 brand JSON 的 logo_image 解析且必须仍在品牌目录内。"""
            if not re.fullmatch(r"[\w\-]+", name or ""):
                self._send(404, "not found", "text/plain")
                return
            d = os.path.realpath(os.path.expanduser(os.environ.get(
                "XMEDIA_BRAND_DIR", "~/.xmedia/brands")))
            try:
                brand = json.load(open(os.path.join(d, name + ".json"),
                                       encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                self._send(404, "not found", "text/plain")
                return
            li = brand.get("logo_image") or ""
            if not li or li.startswith(("http://", "https://")):
                self._send(404, "no local logo", "text/plain")
                return
            img = os.path.realpath(os.path.join(d, li))
            if not img.startswith(d + os.sep) or not os.path.isfile(img):
                self._send(404, "not found", "text/plain")
                return
            ctype = "image/png" if img.endswith(".png") else (
                "image/jpeg" if img.endswith((".jpg", ".jpeg")) else (
                "image/svg+xml" if img.endswith(".svg") else (
                "image/webp" if img.endswith(".webp") else
                "application/octet-stream")))
            with open(img, "rb") as fh:
                self._send(200, fh.read(), ctype)

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
            # video/audio 的 seek 依赖 Range/206（mp4 的 moov 常在尾部，
            # 无 Range 时 Chromium 无法定位 seek 点——Safari audio 同款
            # 约束，参照 _serve_mix）；无 Range 头保持全量 200（旧行为）。
            rng = self.headers.get("Range", "")
            if rng.startswith("bytes="):
                m = re.match(r"(\d*)-(\d*)", rng[6:].split(",")[0].strip())
                if m:
                    total = os.path.getsize(full)
                    start = int(m.group(1)) if m.group(1) else 0
                    end = (int(m.group(2)) if m.group(2)
                           else min(total - 1, start + 1024 * 1024 - 1))
                    end = min(end, total - 1)
                    if start <= end and start < total:
                        with open(full, "rb") as f:
                            f.seek(start)
                            chunk = f.read(end - start + 1)
                        self.send_response(206)
                        self.send_header("Content-Type", ctype)
                        self.send_header("Content-Length", str(len(chunk)))
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header(
                            "Content-Range", f"bytes {start}-{end}/{total}")
                        self.end_headers()
                        try:
                            self.wfile.write(chunk)
                        except (BrokenPipeError, ConnectionResetError):
                            pass
                        return
            with open(full, "rb") as f:
                self._send(200, f.read(), ctype)

    return Handler


def home_html(projects):
    """Home 页（项目管理）：Make Something 三步向导 + 项目卡片网格。

    对齐 HF first-cut 的「What do you want to make? → A bit about you →
    合成首剪」形态：类型宫格选叙事 Arc → 一句话 brief（受众/语气可选）
    → 品牌 kit 可选 → /api/project/create 一键建项目跳编辑器。
    独立小页面（不进 web 构建），风格对齐 studio tokens。"""
    cards = "".join(
        f'<a class="pj" href="/project/{p["name"]}">'
        f'<div class="pj-thumb"><img src="/api/project-thumb/{p["name"]}"'
        f' alt="" onerror="this.classList.add(\'miss\')"></div>'
        f'<div class="pj-name">{p["name"]}</div>'
        f'<div class="pj-meta">{p["scenes"]} 场景'
        + (f' · {p["arc"]}' if p["arc"] else "")
        + f' · {p["updated"]}</div>'
        f'<div class="pj-msg">{p["message"]}</div></a>'
        for p in projects)
    # 类型宫格（HF survey 的 grid-cols-4 形态）：Arc 清单 → 卡片。
    # tile = 真实渲染帧（/api/arc-preview，后台线程生成；未就绪时
    # onerror 藏图显渐变色条兜底）——HF survey tiles 的本地对应物。
    arc_cards = "".join(
        f'<button type="button" class="mk-arc" data-arc="{a["id"]}">'
        f'<span class="mk-arc-tile">'
        f'<img src="/api/arc-preview/{a["id"]}" alt="" '
        f'onerror="this.classList.add(\'miss\')">'
        f'<span class="mk-arc-strip s-{i % 5}"></span>'
        f'</span>'
        f'<b>{a["label"].split("（")[0]}</b>'
        f'<i>{a["label"].split("（")[1].rstrip("）") if "（" in a["label"] else ""}</i>'
        f'</button>'
        for i, a in enumerate(__import__("hf_storyboard").arcs()))
    # 品牌 kit 下拉由前端 fetch /api/brands 动态填充（default pin 标记）
    return f"""<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>xmedia Studio — 项目</title>
<style>
:root {{ --bg:#0b0b11; --panel:#14141c; --panel2:#1b1b25; --line:#282838;
        --line2:#37374a; --ink:#e9e9f2; --dim:#8b8ba0; --acc:#6d8bff;
        --acc2:#8b6dff; --ok:#3ecf8e; }}
* {{ box-sizing: border-box }}
body {{ margin:0; background:var(--bg); color:var(--ink);
       font:14px/1.5 -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif }}
.wrap {{ max-width: 860px; margin: 0 auto; padding: 40px 20px }}
h1 {{ font-size: 22px; margin: 0 0 4px }}
.sub {{ color: var(--dim); font-size: 12.5px; margin-bottom: 30px }}
/* ---- Make Something 向导（HF first-cut 形态）---- */
.mk {{ background: var(--panel); border: 1px solid var(--line2);
      border-radius: 14px; padding: 20px; margin-bottom: 34px }}
.mk-step {{ font-size: 11.5px; color: var(--dim); letter-spacing: .04em;
      margin-bottom: 10px }}
.mk-grid {{ display: grid; grid-template-columns: repeat(5, 1fr); gap: 10px }}
.mk-arc {{ display: flex; flex-direction: column; gap: 6px;
      background: var(--panel2); border: 1px solid var(--line);
      border-radius: 12px; padding: 12px 10px; cursor: pointer;
      color: var(--ink); text-align: left; font: inherit;
      transition: border-color .12s, transform .12s }}
.mk-arc:hover {{ border-color: var(--dim) }}
.mk-arc.sel {{ border-color: var(--acc);
      box-shadow: 0 0 0 1px var(--acc) }}
.mk-arc:active {{ transform: scale(.98) }}
.mk-arc b {{ font-size: 13px }}
.mk-arc i {{ font-style: normal; font-size: 10.5px; color: var(--dim);
      line-height: 1.4 }}
.mk-arc-tile {{ display: block; position: relative; height: 64px;
      border-radius: 8px; overflow: hidden; background: var(--panel2) }}
.mk-arc-tile img {{ position: absolute; inset: 0; width: 100%;
      height: 100%; object-fit: cover; z-index: 2 }}
.mk-arc-tile img.miss {{ display: none }}
.mk-arc-strip {{ display: flex; height: 100%; min-height: 22px;
      border-radius: 5px; overflow: hidden }}
.mk-arc-strip::before {{ content: ""; flex: 2 }}
.mk-arc-strip::after {{ content: ""; flex: 1 }}
.s-0 {{ background: linear-gradient(90deg, #ffcc00, #00a3ff) }}
.s-1 {{ background: linear-gradient(90deg, #e84b5f, #6d8bff) }}
.s-2 {{ background: linear-gradient(90deg, #3ecf8e, #1a6dff) }}
.s-3 {{ background: linear-gradient(90deg, #8b6dff, #ff661a) }}
.s-4 {{ background: linear-gradient(90deg, #fa9600, #3ecf8e) }}
.mk-fields {{ display: flex; flex-direction: column; gap: 10px;
      margin-top: 14px }}
.mk-fields textarea {{ width: 100%; min-height: 64px; background: var(--panel2);
      color: var(--ink); border: 1px solid var(--line);
      border-radius: 8px; padding: 10px 12px; font: inherit;
      resize: vertical; outline: none }}
.mk-fields textarea:focus {{ border-color: var(--acc) }}
.mk-row {{ display: flex; gap: 10px; flex-wrap: wrap }}
.mk-row select, .mk-row input {{ background: var(--panel2); color: var(--ink);
      border: 1px solid var(--line); border-radius: 8px;
      padding: 8px 10px; font: inherit; outline: none; flex: 1;
      min-width: 140px }}
.mk-go {{ margin-top: 14px; background: linear-gradient(135deg,var(--acc),
      var(--acc2)); border: 0; color: #0d1122; font-weight: 700;
      border-radius: 10px; padding: 11px 26px; font-size: 14px;
      cursor: pointer }}
.mk-go:disabled {{ opacity: .6; cursor: default }}
#msg {{ margin-top: 12px; font-size: 12.5px; color: var(--dim);
        white-space: pre-wrap }}
.guide {{ background: var(--panel); border: 1px solid var(--line);
         border-radius: 12px; padding: 14px 18px; margin-bottom: 34px;
         display: flex; flex-direction: column; gap: 8px }}
.g-step {{ font-size: 12.5px; color: var(--dim) }}
.g-step b {{ display: inline-grid; place-items: center; width: 20px;
            height: 20px; border-radius: 50%; background: var(--acc);
            color: #0d1122; font-size: 11px; margin-right: 8px }}
.pjs {{ display: grid; grid-template-columns: repeat(auto-fill, minmax(240px,1fr));
        gap: 14px }}
.pj {{ display: block; background: var(--panel); border: 1px solid var(--line2);
       border-radius: 12px; padding: 10px 14px; text-decoration: none;
       color: var(--ink); transition: border-color .12s }}
.pj-thumb {{ margin: -2px -6px 10px; border-radius: 9px; overflow: hidden;
       background: linear-gradient(135deg, #16213e, #0f3460); min-height: 96px; }}
.pj-thumb img {{ display: block; width: 100%; height: 110px;
       object-fit: cover; }}
.pj-thumb img.miss {{ display: none; }}
.pj:hover {{ border-color: var(--acc) }}
.pj-name {{ font-weight: 600; font-size: 14px }}
.pj-meta {{ font-size: 11px; color: var(--dim); margin: 6px 0 }}
.pj-msg {{ font-size: 12px; color: var(--dim); overflow: hidden;
           text-overflow: ellipsis; white-space: nowrap }}
.empty {{ color: var(--dim); font-size: 13px }}
</style></head><body><div class="wrap">
<h1>xmedia Studio</h1>
<div class="sub">做个东西 → 分镜审阅 → 导出 MP4（全部本地、确定性管线）</div>
<div class="mk">
  <div class="mk-step" id="mk-step-label">1 of 3 · 你想做个什么？</div>
  <div class="mk-grid" role="group" aria-label="你想做个什么？">{arc_cards}</div>
  <div class="mk-fields" id="mk-fields" style="display:none">
    <div class="mk-step">2 of 3 · 这条片子说什么</div>
    <textarea id="message" placeholder="一句话说你要做什么视频，例如：把切片发布从半天缩短到一分钟「切片工作流」"></textarea>
    <div class="mk-row">
      <input id="audience" placeholder="受众（可选）">
      <input id="mood" placeholder="音乐/能量（可选）">
    </div>
    <div class="mk-step" style="margin-top:8px">3 of 3 · 品牌（可选）</div>
    <div class="mk-row">
      <select id="brand"><option value="">不用品牌（默认色板）</option></select>
      <input id="name" placeholder="项目名（可选，默认自动）" style="flex:1">
    </div>
    <button class="go mk-go" id="create">合成首剪</button>
  </div>
  <div id="msg"></div>
</div>
<div class="guide">
  <div class="g-step"><b>1</b> 选一个叙事类型 → 一句话 brief →「合成首剪」（5 个商业叙事 Arc）</div>
  <div class="g-step"><b>2</b> 编辑页左栏「对话」说一句话改内容 ·「分镜」逐镜看真实帧（卡上直接改文案/时长）</div>
  <div class="g-step"><b>3</b> 顶栏「导出 MP4」选画质/格式/帧率出片（全部本地、确定性管线）</div>
</div>
<h2 style="font-size:15px">项目</h2>
<div class="pjs">{cards or '<div class="empty">还没有项目——上面选个类型开做</div>'}</div>
</div>
<script>
(function () {{
  var arc = null;
  var grid = document.querySelector(".mk-grid");
  var fields = document.getElementById("mk-fields");
  var label = document.getElementById("mk-step-label");
  grid.addEventListener("click", function (ev) {{
    var btn = ev.target.closest(".mk-arc");
    if (!btn) return;
    arc = btn.dataset.arc;
    grid.querySelectorAll(".mk-arc").forEach(function (b) {{
      b.classList.toggle("sel", b === btn);
    }});
    fields.style.display = "flex";
    label.textContent = "已选「" + btn.querySelector("b").textContent +
                        "」· 2 of 3 这条片子说什么";
    /* sentence 引导（HF 把挑选合成一句话的本地对应）：按 Arc 换
       完整例句作 placeholder——用户改句子而不是从空白填表 */
    document.getElementById("message").placeholder = {{
      "launch": "给{{受众}}的一条 20 秒发布片：{{钩子}}「{{一句话主张}}」→ 三点证明 → 立即开始",
      "problem-solution": "{{受众}}还在被{{旧流程}}拖慢？我们把它从{{耗时}}缩到{{结果}}",
      "listicle": "换掉{{旧方案}}的三个理由：{{理由一}} · {{理由二}} · {{理由三}}",
      "explainer": "一分钟讲清{{主题}}：它是什么 → 为什么重要 → 怎么用",
      "comparison": "还在用{{旧方案}}？{{新方案}}在{{关键指标}}上快{{N}}倍"
    }}[arc] || "一句话说你要做什么视频";
    document.getElementById("message").focus();
  }});
  /* ?new=1（编辑器「＋ New video」入口）：直接聚焦向导 */
  if (location.search.indexOf("new=1") >= 0) {{
    document.querySelector(".mk").scrollIntoView({{ behavior: "smooth" }});
    document.getElementById("message").focus();
  }}
  /* 品牌 kits 注入（/api/brands 同源数据，default pin 标记） */
  fetch("/api/brands").then(function (r) {{ return r.json(); }}).then(function (r) {{
    var sel = document.getElementById("brand");
    (r.brands || []).forEach(function (b) {{
      var o = document.createElement("option");
      o.value = b.name;
      o.textContent = b.name + (b._default ? "（默认）" : "");
      sel.appendChild(o);
    }});
  }}).catch(function () {{}});
  document.getElementById("create").onclick = function () {{
    if (!arc) {{ document.getElementById("msg").textContent = "先选一个类型"; return; }}
    var body = {{ message: document.getElementById("message").value,
                 arc: arc,
                 audience: document.getElementById("audience").value,
                 mood: document.getElementById("mood").value,
                 name: document.getElementById("name").value,
                 brand: document.getElementById("brand").value }};
    if (!body.message.trim()) {{
      document.getElementById("msg").textContent = "说一句话你要做什么";
      document.getElementById("message").focus();
      return;
    }}
    var btn = this, msg = document.getElementById("msg");
    btn.disabled = true;
    msg.textContent = "合成中…（叙事骨架 → 占位文案 → 契约页）";
    fetch("/api/project/create", {{ method: "POST",
      headers: {{ "Content-Type": "application/json" }},
      body: JSON.stringify(body) }})
      .then(function (r) {{ return r.json(); }})
      .then(function (r) {{
        if (r.error) {{
          msg.textContent = "失败：" + r.error;
          btn.disabled = false;
          return;
        }}
        location.href = r.url + "?fresh=1";
      }})
      .catch(function (e) {{
        msg.textContent = "失败：" + e.message;
        btn.disabled = false;
      }});
  }};
}})();
</script></body></html>"""


def serve(contract_path, spec_path, port, no_sfx, home=False):
    contract_path = os.path.abspath(contract_path) if contract_path else None
    if contract_path is None:
        # Home 模式：项目管理页起步，open_project 在点开项目时重绑。
        srv = StudioServer(None, None, port, no_sfx, home=True)
    else:
        spec_dir = os.path.dirname(contract_path) or "."
    if spec_path is None and contract_path is not None:
        # 契约页生成器注释携带 spec 文件名（hf_compose 写入）——自动发现
        m = re.search(r'spec:([^|]+)\|', _read_head(contract_path))
        if m:
            candidate = os.path.join(os.path.dirname(contract_path),
                                     m.group(1).strip())
            if os.path.isfile(candidate):
                spec_path = candidate
    if contract_path is None:
        # Home 模式：上面那段自动发现跳过，spec 维持 None。
        srv = StudioServer(None, None, port, no_sfx, home=True)
        # Arc 示例真帧后台生成（Home 宫格的"画面墙"，几秒后就位；
        # 已缓存则跳过）。
        threading.Thread(target=srv._ensure_arc_previews,
                         daemon=True).start()
    else:
        srv = StudioServer(contract_path, spec_path, port, no_sfx)

    class PassThrough(ThreadingHTTPServer):
        daemon_threads = True

    httpd = PassThrough(("127.0.0.1", port), make_handler(srv))
    port = httpd.server_address[1]
    if contract_path:
        print(f"xmedia Studio: http://127.0.0.1:{port}  "
              f"(contract: {os.path.basename(contract_path)}, "
              f"spec: {srv.spec_path or '未绑定——只读预览'})  Ctrl+C 退出")
    else:
        print(f"xmedia Studio (Home): http://127.0.0.1:{port}  "
              "Ctrl+C 退出")
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
    parser.add_argument("html", nargs="?", default=None,
                        help="契约 HTML（hf_compose 产出或过校验的手写页）；"
                             "--home 模式可省略")
    parser.add_argument("-o", "--output", default=None,
                        help="注入模式：输出 studio HTML（默认 <stem>.studio.html）")
    parser.add_argument("--serve", action="store_true",
                        help="启动本地 Studio 服务（浏览器 UI：编辑 spec → "
                             "热重组 → TTS → 导出）")
    parser.add_argument("--home", action="store_true",
                        help="Home 模式：项目管理页起步（一句话新建项目 → "
                             "叙事 Arc 骨架 → 进编辑）；项目存 "
                             "~/.xmedia/projects/")
    parser.add_argument("--spec", default=None,
                        help="服务模式：绑定的 spec JSON 路径（缺省从契约页"
                             "生成器注释自动发现；无则只读预览）")
    parser.add_argument("--port", type=int, default=8120)
    parser.add_argument("--no-sfx", action="store_true",
                        help="服务模式：重组时关闭 SFX")
    args = parser.parse_args()

    if args.serve:
        if args.home:
            if args.html:
                print("xmedia: --home 与契约页互斥（项目页直接给路径）",
                      file=sys.stderr)
                return 2
            sys.exit(serve(None, None, args.port, args.no_sfx, home=True))
        if not args.html:
            parser.error("缺少契约 HTML（或用 --home 进项目管理模式）")
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
