/* ---------------- 中央 Storyboard 网格（HF 预览区 Main/Storyboard
 * 双 tab 的 Storyboard 形态）----------------
 * 预览区顶部双 tab：Main = 成片预览；Storyboard = 全镜大图网格总览
 * （与左栏逐镜审阅分工：左栏看节奏/内联编辑，中央一屏看全部镜头的
 * 叙事流）。画面填充复用 panels/storyboard.js 的 fillSceneVisual；
 * 点卡 = 选中该镜 + seek 到镜头起点（Main 回去即看到该帧）。 */
import { $, escHtml, log } from "../shared/dom.js";
import { mk } from "../timeline/model.js";
import { selectScene } from "../panels/scenes.js";
import { seekTo } from "./driver.js";
import { fitPreview } from "./zoom.js";
import { sceneFlatText, fillSceneVisual } from "../panels/storyboard.js";

let sbGridOn = false;

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
