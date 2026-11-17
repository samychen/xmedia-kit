import { fps } from "../shared/time.js";

/* ---------------- 时间轴 ---------------- */
export var TRACKS = [
  { kind: "scene", name: "场景 / Video", row: "v-scene" },
  { kind: "caption", name: "字幕 / Captions", row: "v-caption" },
  { kind: "narration", name: "旁白 / Voice", row: "v-audio" },
  { kind: "sfx", name: "音效 / SFX", row: "v-audio" },
  { kind: "bgm", name: "音乐 / BGM", row: "v-audio" }
];
export var CLIP_PALETTE = ["#1a2a4e", "#252046", "#183c37", "#3d2a1c",
                    "#202f4a", "#2c1f3d"];

/* 全局字幕段（ASR / SRT 写进 spec.captions.segments）与场景内文本是两套
   数据：字幕轨必须优先画 segments，否则 ASR 出字后轨道仍是空的。 */
export function capSegments() {
  var caps = (state.spec && state.spec.captions) || {};
  if (caps.enabled === false) return [];
  return (caps.segments || []).filter(function (s) {
    return s && String(s.text || "").trim() !== "";
  });
}
export function sceneTexts() {
  return (state.spec.scenes || []).map(function (spec) {
    return spec.subtitle || spec.text || spec.quote ||
           (spec.items ? spec.items.join(" · ") : "");
  });
}
export function capTrackSub() {
  var n = capSegments().length;
  if (n) return n + " 段 · captions.segments";
  var m = sceneTexts().filter(function (t) { return t; }).length;
  return m ? m + " 段 · scenes[].text" : "无字幕";
}

export function mk(tag, cls, html) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (html !== undefined) e.innerHTML = html;
  return e;
}
export function hashSeed(s) {
  var h = 0;
  for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
  return Math.abs(h) % 1000 / 37;
}
/* 确定性波形：同一素材永远得到同一条曲线，不依赖随机数。 */
export function waveSVG(seed, w, h, dual) {
  var n = Math.max(12, Math.min(220, Math.round(w / 4)));
  function shape(cy, amp) {
    var up = [], dn = [];
    for (var i = 0; i <= n; i++) {
      var x = (i / n * w).toFixed(1);
      var a = amp * (0.18 + 0.82 * Math.abs(
        Math.sin(i * 0.57 + seed) * Math.cos(i * 0.21 + seed * 1.7)));
      up.push(x + "," + (cy - a).toFixed(1));
      dn.unshift(x + "," + (cy + a).toFixed(1));
    }
    return "M" + up.concat(dn).join(" L") + " Z";
  }
  var paths = dual
    ? '<path d="' + shape(h * .27, h * .23) + '"/>' +
      '<path d="' + shape(h * .73, h * .23) + '"/>'
    : '<path d="' + shape(h * .5, h * .44) + '"/>';
  return '<svg viewBox="0 0 ' + Math.max(8, Math.round(w)) + " " + h +
         '" preserveAspectRatio="none" fill="#49d985" opacity=".92">' +
         paths + "</svg>";
}
/* 真实峰值波形：peaks 是 [0..1] 的数组，按柱高画。
   与 waveSVG 的区别是它反映素材里真实的声音——有没有说话、在哪炸了一下，
   一眼能看出来。示意波形（hashSeed）只在没有音轨时兜底。 */
export function peaksSVG(peaks, w, h, dual) {
  var n = peaks.length;
  if (!n) return "";
  var bw = w / n;
  var bars = "";
  for (var i = 0; i < n; i++) {
    var a = Math.max(0.02, peaks[i]);
    var x = (i * bw).toFixed(1);
    var bh = (a * h * 0.46).toFixed(1);
    var y1 = (h * 0.5 - bh).toFixed(1);
    bars += '<rect x="' + x + '" y="' + y1 + '" width="' +
            Math.max(1, bw * 0.72).toFixed(1) + '" height="' +
            (bh * 2).toFixed(1) + '" rx="0.5"/>';
  }
  return '<svg viewBox="0 0 ' + Math.max(8, Math.round(w)) + " " + h +
         '" preserveAspectRatio="none" fill="#49d985" opacity=".92">' +
         bars + "</svg>";
}

/* 拉真实峰值：成功则替换该块的波形；无音轨/失败则保留示意波形。 */
export var waveCache = {};
export function loadWaveForClip(holder, src, w, h, dual, start, dur) {
  if (!src || !holder) return;
  var key = src + "|" + Math.round(w) + "|" + (start || 0).toFixed(3) +
            "|" + (dur || 0).toFixed(3);
  holder.setAttribute("data-wave-key", key);
  function apply(svg) {
    if (!svg) return;
    // 不能直接用 holder 引用：renderTimeline 会 grid.innerHTML="" 整块重建，
    // 异步回来时旧节点已脱离文档（isConnected=false），渲染就丢了。
    // 按 key 在**当前** DOM 里重新查找，才是这次渲染的那一格。
    var live = document.querySelector(
      '.wave[data-wave-key="' + CSS.escape(key) + '"]');
    if (live) live.innerHTML = svg;
  }
  if (waveCache[key] !== undefined) { apply(waveCache[key]); return; }
  var url = "/api/waveform?path=" + encodeURIComponent(src) +
            "&n=48&s=" + (start || 0).toFixed(3) + "&d=" + (dur || 0).toFixed(3);
  fetch(url).then(function (r) { return r.json(); }).then(function (res) {
    var svg = (res && res.ok && res.peaks) ? peaksSVG(res.peaks, w, h, dual) : "";
    waveCache[key] = svg;
    apply(svg);
  }).catch(function () { waveCache[key] = ""; });
}

/* 程序化 filmstrip：按场景 bg / 类型铺连续"画面帧"，零外部依赖。 */
export function stripHTML(sc, idx, wpx) {
  var spec = (state.spec.scenes || [])[idx] || {};
  var bg = /^#[0-9a-fA-F]{6}$/.test(spec.bg || "")
           ? spec.bg : CLIP_PALETTE[idx % CLIP_PALETTE.length];
  var typ = (sc.label || "").replace(/^scene-/, "");
  var n = Math.max(2, Math.min(90, Math.floor(wpx / 28)));
  var out = "";
  for (var i = 0; i < n; i++) {
    out += '<span class="fr ' + typ + '" style="background-color:' + bg +
           '"></span>';
  }
  return '<div class="strip">' + out + "</div>";
}

/* 视频场景缩略图：从源视频抽帧，按 scene 铺成连续缩略图条。结果按
   spec.video 路径缓存，避免每次 renderTimeline 重复请求 / 抽帧。 */
export var thumbCache = {};
export function paintThumbs(holder, frames) {
  holder.innerHTML = "";
  holder.classList.remove("loading");
  holder.classList.add("vid");
  for (var i = 0; i < frames.length; i++) {
    var im = document.createElement("img");
    im.src = frames[i];
    im.className = "tb";
    im.alt = "";
    im.loading = "lazy";
    holder.appendChild(im);
  }
}
/* 缩略图按片段自己的源内窗口取样：切割后的后段从 video_from 起抽帧，
   否则前后两段缩略图一模一样，看不出刀口落在哪。 */
export function loadThumbsForClip(holder, videoPath, from, dur) {
  if (!videoPath) return;
  var f = Math.max(0, Number(from) || 0);
  var d = Math.max(0, Number(dur) || 0);
  var key = videoPath + "|" + f.toFixed(3) + "|" + d.toFixed(3);
  if (thumbCache[key]) {
    paintThumbs(holder, thumbCache[key]);
    return;
  }
  holder.classList.add("loading");
  fetch("/api/thumbs?path=" + encodeURIComponent(videoPath) +
        "&n=10&w=480&h=270&s=" + f.toFixed(3) + "&d=" + d.toFixed(3))
    .then(function (r) { return r.json(); })
    .then(function (res) {
      if (res && res.frames && res.frames.length) {
        thumbCache[key] = res.frames;
        if (document.body.contains(holder)) {
          paintThumbs(holder, res.frames);
        }
      } else {
        holder.classList.remove("loading");
      }
    })
    .catch(function () { holder.classList.remove("loading"); });
}












































export function snapTime(t) {
  var f = Math.round(t * fps()) / fps(), best = f, d0 = 0.085;
  state.scenes.forEach(function (s) {
    [s.start, s.start + s.dur].forEach(function (b) {
      var d = Math.abs(b - t);
      if (d < d0) { d0 = d; best = b; }
    });
  });
  return Math.max(0, Math.min(state.duration, best));
}

export function splitSceneData(sc, cut, total) {
  var tail = structuredClone(sc);
  var c = Math.round(cut * 100) / 100;
  sc.duration = c;
  tail.duration = Math.round((total - c) * 100) / 100;
  // 接缝必须是硬切：两段是同一素材的连续画面，继承来的入场动画 /
  // 转场会让后半段在刀口再淡入一次（画面内容没变，看起来就是"转场
  // 没反应"，总时长还会被转场重叠吃掉）。要过渡就在属性面板里给
  // 后半段显式选一个转场。
  tail.transition = "none";
  tail.animation = "none";
  if (sc.video) {
    var base = Number(sc.video_from);
    if (!base && sc.video && typeof sc.video === "object") {
      base = Number(sc.video.from) || 0;
    }
    base = base || 0;
    if (tail.video && typeof tail.video === "object") delete tail.video.from;
    tail.video_from = Math.round((base + c) * 1000) / 1000;
  }
  return tail;
}
