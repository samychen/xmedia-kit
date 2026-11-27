/* ---------------- 中央 Storyboard 网格（HF 预览区 Main/Storyboard
 * 双 tab 的 Storyboard 形态）----------------
 * 预览区顶部双 tab：Main = 成片预览；Storyboard = 全镜大图网格总览
 * （一屏看全部镜头的叙事流）。
 * 点卡 = 选中该镜（联动 Inspector）+ seek 到镜头起点（Main 回去即看到该帧）。 */
import { $, escHtml } from "../shared/dom.js";
import { api } from "../shared/api-client.js";
import { loadThumbsForClip, mk } from "../timeline/model.js";
import { selectScene } from "../panels/scenes.js";
import { seekTo } from "./driver.js";
import { fitPreview } from "./zoom.js";

let sbGridOn = false;

/* 无 video 场景的真实帧：html_bake 低分辨率全段烘焙 → 场景中点抽帧
 * （服务端 /api/sb-frames，契约页 mtime 失效，URL 自带版本号——重组合
 * 成后浏览器缓存自然失效）。 */
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

export function sceneFlatText(sc) {
  return sc.subtitle || sc.text || sc.quote ||
      (Array.isArray(sc.items) ? sc.items.join(" · ") : "") ||
      "（无文案）";
}

/* 场景画面填充：视频场景 4 连拍网格；无视频场景先色块+文案兜底，再异步换 /api/sb-frames 真实帧 */
export function fillSceneVisual(shot, sc, i, dur) {
  var media = sceneMedia(sc);
  var bg = /^#[0-9a-fA-F]{6}$/.test(sc.bg || "") ? sc.bg : "#23232f";
  if (!media) shot.classList.add("flat");
  shot.style.background = media ? "" : bg;
  if (media) {
    loadThumbsForClip(shot, media.src, media.from, dur);
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

export function sbGridShow(on) {
  sbGridOn = !!on;
  var view = $("sbgrid-view");
  if (!view) return;
  view.style.display = sbGridOn ? "flex" : "none";
  $("pvwrap").style.display = sbGridOn ? "none" : "flex";
  if (sbGridOn) {
    sbGridRender();
  } else {
    fitPreview();   // Main 画布期间被隐藏过，回来重算缩放
  }
}

export function sbGridIsOn() {
  return sbGridOn;
}

export function sbGridRender() {
  var view = $("sbgrid-view");
  if (!view || !sbGridOn) return;
  var scenes = (state.spec && state.spec.scenes) || [];
  var metas = state.scenes || [];
  view.replaceChildren();

  var head = mk("div", "sbgrid-head");
  head.innerHTML = scenes.length
    ? "<b>Storyboard</b><span>" + scenes.length + " 镜 · 总长 " +
      Number(state.duration || 0).toFixed(1) + "s · 点镜头定位到 Main</span>"
    : "<b>Storyboard</b>";
  view.appendChild(head);

  if (!scenes.length) {
    var empty = mk("div", "sb-empty");
    empty.textContent = "spec 中还没有场景";
    view.appendChild(empty);
    return;
  }

  /* 帧图由 fillSceneVisual 内部经 sbFrames() 拉取（色块兜底，异步替换） */
  var grid = mk("div", "sbgrid");
  scenes.forEach(function (sc, i) {
    var meta = metas[i] || {};
    var dur = meta.dur || sc.duration || 3;
    var card = mk("div", "sbgrid-card" + (i === selIdx ? " sel" : ""));

    var shot = mk("div", "sbgrid-shot sb-shot");
    fillSceneVisual(shot, sc, i, dur);
    card.appendChild(shot);

    var cap = mk("div", "sbgrid-cap");
    cap.innerHTML =
      '<span class="sc-idx">' + (i < 9 ? "0" : "") + (i + 1) + "</span>" +
      '<span class="sb-tc">' + (meta.start || 0).toFixed(1) + "s · " +
      dur.toFixed(1) + "s</span>" +
      '<span class="sb-type">' + escHtml(sc.type || "text") + "</span>";
    card.appendChild(cap);

    var txt = mk("div", "sbgrid-text");
    txt.textContent = sceneFlatText(sc);
    card.appendChild(txt);

    /* 叙事徽标（hook/social_proof/cta…）——Storyboard 看的就是叙事流 */
    var frame = ((state.spec.storyboard || {}).frames || [])[i];
    if (frame && frame.type) {
      var badges = mk("div", "sb-narrative");
      badges.innerHTML =
        '<span class="sb-badge t">' + escHtml(frame.type) + "</span>" +
        (frame.persuasion ? '<span class="sb-badge p">' +
          escHtml(frame.persuasion) + "</span>" : "") +
        (frame.beat ? '<span class="sb-badge b">' +
          escHtml(frame.beat) + "</span>" : "");
      card.appendChild(badges);
    }

    card.style.cursor = "pointer";
    card.onclick = function () {
      selectScene(i, true);
      seekTo(meta.start || 0);
      grid.querySelectorAll(".sbgrid-card.sel").forEach(function (c) {
        c.classList.remove("sel");
      });
      card.classList.add("sel");
    };
    grid.appendChild(card);
  });
  view.appendChild(grid);
}
