/* Storyboard 逐镜审阅（对齐 HF Studio app first-video 的
 * "Approve the storyboard" 步骤，本地实现）：
 *
 * 每镜一张确认卡 = 真实帧缩略图（/api/thumbs 现有管线，n=4 连拍）
 * + 时机 + 场景文案 + 「改这镜」（selectScene 定位 Inspector + seek 到
 * 镜头起点）。改完点「重新生成缩略图」或再次打开即可刷新——审阅是
 * 只读视图，不复制编辑逻辑；批准 = 关闭卡片直接重组/导出。
 *
 * 与场景面板（renderSceneList）的分工：场景卡是常驻导航（色块+文字，
 * 短小滚动列表），storyboard 是**渲染前的逐镜大图审阅**（连拍网格看
 * 每镜内部节奏）。二者共享 selectScene 定位链。
 */

import { $ } from "../shared/dom.js";
import { escHtml } from "../shared/dom.js";
import { selectScene } from "./scenes.js";
import { seekTo } from "../preview/driver.js";
import { loadThumbsForClip, mk } from "../timeline/model.js";

let sbOpen = false;

function sceneMedia(sc) {
  var v = sc.video;
  if (!v) return null;
  var src = typeof v === "string" ? v : String(v.src || "");
  if (!src || /^https?:/i.test(src)) return null; // 站外源不走本地 thumbs
  return { src: src, from: Number(sc.video_from) || 0 };
}

function shotGrid(holder, media, dur) {
  // 连拍网格：n=4 覆盖整镜，看镜头内部节奏（对应 HF "every shot as a
  // still with its timing"）。失败静默——网格保持 loading 态即可。
  loadThumbsForClip(holder, media.src, media.from, dur);
}

export function storyboardOpen(on) {
  sbOpen = !!on;
  var panel = $("sb-panel");
  if (!panel) return;
  panel.style.display = sbOpen ? "flex" : "none";
  if (sbOpen) storyboardRender();
}

export function storyboardIsOpen() {
  return sbOpen;
}

export function storyboardRender() {
  var list = $("sb-list");
  if (!list) return;
  var scenes = (state.spec && state.spec.scenes) || [];
  var metas = state.scenes || [];
  $("sb-count").textContent = scenes.length
    ? scenes.length + " 镜 · 总长 " + (state.duration || 0).toFixed(1) + "s"
    : "";
  list.innerHTML = "";
  if (!scenes.length) {
    list.innerHTML = '<div class="sb-empty">spec 中还没有场景</div>';
    return;
  }
  scenes.forEach(function (sc, i) {
    var meta = metas[i] || {};
    var dur = meta.dur || sc.duration || 3;
    var bg = /^#[0-9a-fA-F]{6}$/.test(sc.bg || "") ? sc.bg : "#23232f";
    var card = mk("div", "sb-card" + (i === selIdx ? " sel" : ""));
    card.dataset.i = String(i);

    var head = mk("div", "sb-head");
    head.innerHTML =
        '<span class="sc-idx">' + (i < 9 ? "0" : "") + (i + 1) + "</span>" +
        '<span class="sb-tc">' + (meta.start || 0).toFixed(2) + "s → " +
        ((meta.start || 0) + dur).toFixed(2) + "s · " + dur.toFixed(1) + "s</span>" +
        '<span class="sb-type">' + escHtml(sc.type || "text") + "</span>";
    card.appendChild(head);

    // 画面：视频场景连拍网格；无视频用背景色块+文案预览
    var media = sceneMedia(sc);
    var shot = mk("div", "sb-shot" + (media ? "" : " flat"));
    shot.style.background = media ? "" : bg;
    if (media) {
      shotGrid(shot, media, dur);
    } else {
      var txt = mk("div", "sb-flat-text");
      txt.textContent = sceneFlatText(sc);
      shot.appendChild(txt);
    }
    card.appendChild(shot);

    var foot = mk("div", "sb-foot");
    foot.innerHTML =
        '<div class="sb-text">' + escHtml(sceneFlatText(sc)) + "</div>";
    var edit = mk("button", "btn sb-edit");
    edit.type = "button";
    edit.textContent = "改这镜";
    edit.setAttribute("aria-label", "定位到第 " + (i + 1) + " 镜编辑");
    edit.onclick = function () {
      selectScene(i, true);          // Inspector + 时间轴 + 预览高亮
      storyboardOpen(false);         // 审阅完跳去编辑，卡片收起
    };
    foot.appendChild(edit);
    card.appendChild(foot);
    list.appendChild(card);
  });
}

function sceneFlatText(sc) {
  return sc.subtitle || sc.text || sc.quote ||
      (Array.isArray(sc.items) ? sc.items.join(" · ") : "") ||
      "（无文案）";
}
