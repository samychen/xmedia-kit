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
import { escHtml, log } from "../shared/dom.js";
import { api } from "../shared/api-client.js";
import { refresh } from "./status.js";
import { selectScene } from "./scenes.js";
import { seekTo } from "../preview/driver.js";
import { loadThumbsForClip, mk } from "../timeline/model.js";

let sbOpen = false;
/* 无 video 场景的真实帧：html_bake 低分辨率全段烘焙 → 场景中点抽帧
 * （服务端 /api/sb-frames，契约页 mtime 失效，URL 自带版本号——重组合
 * 成后浏览器缓存自然失效）。每次 render 都拉（服务端命中缓存很快）。 */
export function sbFrames() {
  return api("/api/sb-frames");
}

export function sceneMedia(sc) {
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
  list.replaceChildren();
  if (!scenes.length) {
    list.innerHTML = '<div class="sb-empty">spec 中还没有场景</div>';
    return;
  }
  scenes.forEach(function (sc, i) {
    var meta = metas[i] || {};
    var dur = meta.dur || sc.duration || 3;
    var card = mk("div", "sb-card" + (i === selIdx ? " sel" : ""));
    card.dataset.i = String(i);

    var head = mk("div", "sb-head");
    head.innerHTML =
        '<span class="sc-idx">' + (i < 9 ? "0" : "") + (i + 1) + "</span>" +
        '<span class="sb-tc">' + (meta.start || 0).toFixed(2) + "s → " +
        ((meta.start || 0) + dur).toFixed(2) + "s · " + dur.toFixed(1) + "s</span>" +
        '<span class="sb-type">' + escHtml(sc.type || "text") + "</span>";
    card.appendChild(head);

    // 叙事徽标（spec.storyboard.frames 同索引）：type = 商业任务
    // （hook/social_proof/cta…）、persuasion = 修辞、beat = 情绪节拍
    // ——HF Storyboard 工作流的词汇表，让分镜从"看画面"升级为
    // "看这镜在整片叙事里干什么"。
    var sbFrame = ((state.spec.storyboard || {}).frames || [])[i];
    if (sbFrame && sbFrame.type) {
        var badges = mk("div", "sb-narrative");
        badges.innerHTML =
            '<span class="sb-badge t">' + escHtml(sbFrame.type) + "</span>" +
            (sbFrame.persuasion
                ? '<span class="sb-badge p">' +
                  escHtml(sbFrame.persuasion) + "</span>" : "") +
            (sbFrame.beat
                ? '<span class="sb-badge b">' + escHtml(sbFrame.beat) +
                  "</span>" : "");
        card.appendChild(badges);
    }

    // 画面：视频场景连拍网格；无视频场景用服务端渲染的真实帧
    // （色块+文案兜底），拿到即替换——共用 fillSceneVisual。
    var shot = mk("div", "sb-shot");
    fillSceneVisual(shot, sc, i, dur);
    card.appendChild(shot);

    // 点画面 = 选中该镜（Inspector 联动，分镜保持打开继续看）——
    // 图像 UI 的自然交互，此前点击无响应是「不知道怎么用」的根因之一。
    shot.style.cursor = "pointer";
    shot.onclick = function () {
      selectScene(i, false);   // 选中联动 Inspector/时间轴/预览高亮，不跳走
      list.querySelectorAll(".sb-card.sel").forEach(function (c) {
        c.classList.remove("sel");
      });
      card.classList.add("sel");
    };

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

    // 内联编辑：文案 + 时长直接在卡上改（「应用」走 /api/recompose，
    // 重组后真实帧按契约页 mtime 自动重渲）。有旁白的场景文案锁定
    // （音画同步，与 Inspector 一致），时长始终可改。
    var locked = !!sc.narration;
    var editor = mk("div", "sb-editor");
    editor.innerHTML =
        '<textarea class="sb-edit-text" rows="2" placeholder="这一镜的文案…"'
        + (locked ? " disabled" : "") + ">"
        + escHtml(sc.text || sceneFlatText(sc)) + "</textarea>"
        + '<div class="sb-edit-row">'
        + '<span class="sb-edit-label">时长</span>'
        + '<input class="sb-edit-dur" type="number" step="0.1" min="0.5"'
        + ' value="' + Number(dur).toFixed(1) + '">s'
        + '<span class="sp"></span>'
        + '<button class="btn sb-apply" type="button">应用</button>'
        + "</div>"
        + (locked ? '<div class="sb-edit-note">🔒 有旁白，文案锁定——'
                   + "改词用 Ask AI（会保持音画同步指引）</div>" : "");
    card.appendChild(editor);
    editor.querySelector(".sb-apply").onclick = function () {
      var newText = editor.querySelector(".sb-edit-text").value.trim();
      var newDur = parseFloat(editor.querySelector(".sb-edit-dur").value);
      if (isNaN(newDur) || newDur < 0.5 || newDur > 60) {
        log("时长需在 0.5–60s 之间", "err");
        return;
      }
      state.spec.scenes[i].duration = newDur;
      if (!locked) {
        state.spec.scenes[i].text = newText;
      }
      api("/api/recompose", { spec: state.spec,
                              no_sfx: !document.querySelector("#sfx").checked })
        .then(function () {
          log("第 " + (i + 1) + " 镜已应用（文案" +
              (locked ? "锁定" : "已更新") + " · 时长 " +
              newDur.toFixed(1) + "s），真实帧重新渲染中…", "ok");
        })
        .then(function () { return refresh(true); })
        .then(function () { storyboardRender(); })
        .catch(function (e) {
          log("分镜应用失败：" + e.message, "err");
        });
    };
    list.appendChild(card);
  });
}

/* 场景画面填充（左栏逐镜卡与中央 Storyboard 网格共用）：视频场景
 * 连拍网格；无视频场景先色块+文案兜底，再异步换 /api/sb-frames 真实
 * 帧（拿到即替换，帧未就绪时不挡卡片其余内容）。 */
export function fillSceneVisual(shot, sc, i, dur) {
  var media = sceneMedia(sc);
  var bg = /^#[0-9a-fA-F]{6}$/.test(sc.bg || "") ? sc.bg : "#23232f";
  if (!media) shot.classList.add("flat");
  shot.style.background = media ? "" : bg;
  if (media) {
    shotGrid(shot, media, dur);
  } else {
    var txt = mk("div", "sb-flat-text");
    txt.textContent = sceneFlatText(sc);
    shot.appendChild(txt);
    (function (shotEl, idx) {
      sbFrames().then(function (data) {
        var url = data && data.urls ? data.urls[idx] : null;
        if (!url) return;
        var img = new Image();
        img.onload = function () {
          if (shotEl.isConnected && !shotEl.querySelector("img")) {
            shotEl.textContent = "";   // 清掉色块+文字兜底
            shotEl.style.background = "";
            img.style.width = "100%";
            img.style.height = "100%";
            img.style.objectFit = "cover";
            shotEl.appendChild(img);
          }
        };
        img.src = url;
      }).catch(function () { /* 帧渲染失败保持色块兜底 */ });
    })(shot, i);
  }
  return media;
}

export function sceneFlatText(sc) {
  return sc.subtitle || sc.text || sc.quote ||
      (Array.isArray(sc.items) ? sc.items.join(" · ") : "") ||
      "（无文案）";
}
