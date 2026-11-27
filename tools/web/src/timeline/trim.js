import { recompose } from "../panels/status.js";
import { log, $ } from "../shared/dom.js";
import { fps } from "../shared/time.js";
import { splitSceneData } from "./model.js";
import { tlTotal } from "./range.js";
import { renderTimeline } from "./render.js";

export function trimCommit(idx, newDur) {
  var dur = Math.max(0.3, Math.round(newDur * 100) / 100);
  state.spec.scenes[idx].duration = dur;
  /* 乐观渲染（HF 同款）：本地 meta 同步改 + 立即重渲时间轴——不等
     服务端重组往返；recompose 后台跑，完成后 refresh 校正。 */
  var meta = state.scenes[idx];
  var oldDur = meta ? Number(meta.dur) || dur : dur;
  if (meta) {
    meta.dur = dur;
    // 顺序主轨片段延长/缩短时，乐观推进后续主轨片段起点，避免刷新前重叠闪烁
    var sc = state.spec.scenes[idx];
    if ((Number(sc.track) || 0) === 0 && sc.start === undefined) {
      var dd = dur - oldDur;
      for (var j = idx + 1; j < state.scenes.length; j++) {
        var sj = state.spec.scenes[j] || {};
        if ((Number(sj.track) || 0) === 0 && sj.start === undefined && state.scenes[j]) {
          state.scenes[j].start = Math.max(0, (Number(state.scenes[j].start) || 0) + dd);
        }
      }
      state.duration = Math.max(0.1, (Number(state.duration) || 0) + dd);
    }
  }
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderTimeline();
  log("裁剪 scene-" + (idx + 1) + " → " + dur + "s，重组合成…");
  recompose();
}

export function trimLeftCommit(idx, opts) {
  var sc = state.spec.scenes[idx];
  if (!sc) return;
  var dur = Math.max(0.3, Math.round((opts.newDur || 0.3) * 100) / 100);
  sc.duration = dur;
  if (opts.newStart !== undefined && isFinite(opts.newStart)) {
    sc.start = Math.max(0, Math.round(opts.newStart * 100) / 100);
  }
  if (opts.newVideoFrom !== undefined && isFinite(opts.newVideoFrom) && sc.video) {
    var vf = Math.max(0, Math.round(opts.newVideoFrom * 1000) / 1000);
    if (typeof sc.video === "object" && sc.video.from !== undefined) delete sc.video.from;
    if (vf > 0) sc.video_from = vf;
    else delete sc.video_from;
  }
  var meta = state.scenes[idx];
  if (meta) {
    var oldDur = Number(meta.dur) || dur;
    meta.dur = dur;
    if (opts.newStart !== undefined && isFinite(opts.newStart)) {
      meta.start = sc.start;
    } else if ((Number(sc.track) || 0) === 0 && sc.start === undefined) {
      var dd = dur - oldDur;
      for (var j = idx + 1; j < state.scenes.length; j++) {
        var sj = state.spec.scenes[j] || {};
        if ((Number(sj.track) || 0) === 0 && sj.start === undefined && state.scenes[j]) {
          state.scenes[j].start = Math.max(0, (Number(state.scenes[j].start) || 0) + dd);
        }
      }
      state.duration = Math.max(0.1, (Number(state.duration) || 0) + dd);
    }
  }
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderTimeline();
  log("左缘裁剪 scene-" + (idx + 1) + " → " + dur + "s" +
      (sc.start !== undefined ? " @" + sc.start.toFixed(2) + "s" : "") + "，重组合成…");
  recompose();
}

export function reorder(idx, dir) {
  var arr = state.spec.scenes, j = idx + dir;
  if (j < 0 || j >= arr.length) return;
  var t = arr[idx]; arr[idx] = arr[j]; arr[j] = t;
  selIdx = j;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("场景 " + idx + (dir < 0 ? " 前移" : " 后移") + "，重组合成…");
  recompose();
}

/* 拖拽重排落位：把场景 idx 移到插入位 target（0..len，"放到某场景之前"
   语义）。若原本在叠加轨（有 track>0 或 start），拖回主轨时清除绝对定位。 */
export function reorderTo(idx, target) {
  var arr = state.spec.scenes;
  var sc = arr[idx];
  if (!sc) return false;
  var wasOverlay = (Number(sc.track) || 0) !== 0 || sc.start !== undefined;
  if (!wasOverlay && (target === idx || target === idx + 1)) return false;
  if (target < 0 || target > arr.length) return false;
  sc.track = 0;
  delete sc.start;
  var m = arr.splice(idx, 1)[0];
  var ins = target > idx ? target - 1 : target;
  arr.splice(ins, 0, m);
  selIdx = ins;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderTimeline();   // 乐观渲染：重排立即反映（HF 同款），重组后台跑
  log((wasOverlay ? "场景移回主轨第 " : "场景拖到第 ") + (ins + 1) + " 位，重组合成…");
  recompose();
  return true;
}

/* 拖放到叠加轨或在同一叠加轨内自由平移时间起点（设置 track 与 start）。 */
export function moveClipToTrack(idx, trackNum, newStart) {
  var arr = state.spec.scenes;
  var sc = arr[idx];
  if (!sc) return false;
  var tr = Math.max(0, Number(trackNum) || 0);
  var st = Math.max(0, Math.round(newStart * 100) / 100);
  sc.track = tr;
  sc.start = st;
  if (state.scenes[idx]) {
    state.scenes[idx].start = st;
    state.duration = Math.max(state.duration || 0, st + (state.scenes[idx].dur || sc.duration || 3));
  }
  selIdx = idx;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderTimeline();
  log("场景 " + (idx + 1) + " → " + (tr === 0 ? "主轨" : "叠加轨 " + tr) +
      " @" + st.toFixed(2) + "s，重组合成…");
  recompose();
  return true;
}

/* ---------------- 片段切割（刀口 = 播放头） ----------------
   把一段切成前后两段：后半段继承类型 / 样式 / 文字（同一画面继续），视频
   场景把 video_from 累加刀口偏移——渲染侧按 data-media-start 从源视频该
   时刻继续取帧、混音按同一点修剪音轨，等于把一条素材物理切开。
   字幕轨不动：captions.segments 是绝对时间轴上的段，切割不改变总时长。 */
/* 纯数据变换：把场景 sc 在 cut 秒处切开，返回后半段副本（不碰 DOM，
   便于单测）。前后两段时长之和保持不变；视频场景把 video_from 累加到
   刀口——后半段从源视频该时刻继续，对象形式的 video.from 归一成
   video_from（渲染侧只认场景级字段）。 */
export function splitScene(idx, atSec) {
  var arr = (state.spec && state.spec.scenes) || [];
  var sc = arr[idx], s = state.scenes[idx];
  if (!sc || !s) return false;
  if (sc.narration) {
    log("scene-" + idx + " 时长由旁白拟合（narration），不支持切割", "err");
    return false;
  }
  var total = Number(sc.duration) || s.dur;
  var t = (typeof atSec === "number" && isFinite(atSec)) ? atSec : lastTime;
  var minDur = 1 / fps();
  var cut = Math.round((t - s.start) * 100) / 100;
  if (cut < minDur || total - cut < minDur) {
    log("刀口需落在片段内部（" + s.label + " " + s.start.toFixed(2) + " – " +
        (s.start + total).toFixed(2) + "s，播放头 " + t.toFixed(2) + "s）", "err");
    return false;
  }
  var tail = splitSceneData(sc, cut, total);
  arr.splice(idx + 1, 0, tail);
  selIdx = idx + 1;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("切割 scene-" + idx + " @" + t.toFixed(2) + "s → 前 " + sc.duration +
      "s / 后 " + tail.duration + "s" +
      (sc.video ? "（源内起点 " + tail.video_from + "s）" : "") +
      "，接缝为硬切（要过渡就给后半段选转场），重组合成…");
  recompose();
  return true;
}
/* 两段是不是「同一素材的连续画面」（切割接缝）：同一 src，且后段的源内
   起点正好接在前段终点上。这种接缝上 fade 的两侧几乎是同一帧，混合前后
   亮度不变——看起来像"转场没反应"，其实是没东西可淡。 */
export function sameSourceSeam(idx) {
  var arr = (state.spec && state.spec.scenes) || [];
  var cur = arr[idx], prev = arr[idx - 1];
  if (!cur || !prev || !cur.video || !prev.video) return false;
  var src = function (v) { return typeof v === "string" ? v : (v && v.src) || ""; };
  if (src(cur.video) !== src(prev.video)) return false;
  var prevFrom = Number(prev.video_from) ||
                 (typeof prev.video === "object" ? Number(prev.video.from) || 0 : 0);
  var curFrom = Number(cur.video_from) ||
                (typeof cur.video === "object" ? Number(cur.video.from) || 0 : 0);
  var prevEnd = prevFrom + (Number(prev.duration) || 0);
  return Math.abs(curFrom - prevEnd) < 0.05;
}

/* 刀口选择：优先所选片段（且播放头确实落在它内部），否则取播放头所在
   片段——转场重叠时两个片段都覆盖同一时刻，所以先看 selIdx。 */
export function splitAtPlayhead() {
  var t = Math.max(0, Math.min(tlTotal(), lastTime));
  var scenes = state.scenes || [];
  var inside = function (i) {
    var s = scenes[i];
    return s && t > s.start + 1e-6 && t < s.start + s.dur - 1e-6;
  };
  var idx = inside(selIdx) ? selIdx : -1;
  if (idx < 0) {
    for (var i = 0; i < scenes.length; i++) {
      if (inside(i)) { idx = i; break; }
    }
  }
  if (idx < 0) {
    log("播放头不在任何片段内部——先把播放头拖到要切的位置（刀口）", "err");
    return false;
  }
  return splitScene(idx, t);
}
