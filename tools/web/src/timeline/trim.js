import { recompose } from "../panels/status.js";
import { log, $ } from "../shared/dom.js";
import { fps } from "../shared/time.js";
import { splitSceneData } from "./model.js";
import { tlTotal } from "./range.js";
import { renderTimeline } from "./render.js";

export function trimCommit(idx, newDur) {
  state.spec.scenes[idx].duration = Math.round(newDur * 100) / 100;
  /* 乐观渲染（HF 同款）：本地 meta 同步改 + 立即重渲时间轴——不等
     服务端重组往返；recompose 后台跑，完成后 refresh 校正。 */
  var meta = state.scenes[idx];
  if (meta) meta.dur = state.spec.scenes[idx].duration;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderTimeline();
  log("裁剪 scene-" + idx + " → " +
      state.spec.scenes[idx].duration + "s，重组合成…");
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
   语义）。idx 被移除后右侧目标需要归一（target > idx 时 -1）。 */
export function reorderTo(idx, target) {
  var arr = state.spec.scenes;
  if (target === idx || target === idx + 1) return false;
  if (target < 0 || target > arr.length) return false;
  var m = arr.splice(idx, 1)[0];
  var ins = target > idx ? target - 1 : target;
  arr.splice(ins, 0, m);
  selIdx = ins;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderTimeline();   // 乐观渲染：重排立即反映（HF 同款），重组后台跑
  log("场景 " + (idx + 1) + " 拖到第 " + (ins + 1) + " 位，重组合成…");
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
