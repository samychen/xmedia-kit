import { renderInspector } from "./inspector.js";
import { seekTo } from "../preview/driver.js";
import { escHtml, $ } from "../shared/dom.js";
import { HEAD_W } from "../shared/state.js";
import { renderTimeline } from "../timeline/render.js";
import { reorder } from "../timeline/trim.js";

/* ---------------- 场景列表 / Inspector ---------------- */
export var TECH_KEYS = { narration: 1, narration_volume: 1, duration: 1, bg: 1,
                  text_color: 1, animation: 1, transition: 1, video: 1,
                  type: 1, sfx: 1, padding: 1 };
export var FIELD_DEFS = {
  title: [["标题", "text"], ["副标题", "subtitle"]],
  text: [["主文本", "text"], ["子项（每行一条）", "content", "textarea"]],
  quote: [["引文", "quote"], ["署名", "author"]],
  list: [["标题", "text"], ["条目（每行一条）", "items", "textarea"]],
  stats: [["数值", "value"], ["说明", "description"]],
  comparison: [["左列标题", "left"], ["右列标题", "right"],
               ["左列条目（每行一条）", "left_items", "textarea"],
               ["右列条目（每行一条）", "right_items", "textarea"]],
  outro: [["文本", "text"]],
};
export function sceneFields(sc) {
  var def = FIELD_DEFS[sc.type];
  if (def) return def;
  var fields = [];
  for (var k in sc) {
    if (!TECH_KEYS[k] && typeof sc[k] === "string" && sc[k]) {
      fields.push([k, k]);
    }
  }
  return fields;
}
export function sceneSummary(sc) {
  if (!sc) return "";
  var v = sc.text || sc.quote || sc.value || sc.title || sc.subtitle || "";
  if (!v && Array.isArray(sc.content) && sc.content.length) {
    v = sc.content[0].text || "";
  }
  if (!v && Array.isArray(sc.items) && sc.items.length) v = sc.items[0];
  return String(v).replace(/\s+/g, " ").slice(0, 40) || "（无文本）";
}
export function renderSceneList() {
  var list = $("scene-list");
  var scenes = (state.spec && state.spec.scenes) || [];
  var html = "";
  for (var i = 0; i < scenes.length; i++) {
    var sc = scenes[i];
    var meta = state.scenes[i] || {};
    var dur = meta.dur || sc.duration || 3;
    var bg = /^#[0-9a-fA-F]{6}$/.test(sc.bg || "") ? sc.bg : "#23232f";
    html += '<div class="sc-card' + (i === selIdx ? " sel" : "") +
        '" data-i="' + i + '">' +
        '<span class="sc-idx">' + (i < 9 ? "0" : "") + (i + 1) + '</span>' +
        '<span class="sc-swatch" style="background:' + bg + '"></span>' +
        '<div class="sc-meta"><div class="sc-title">' +
        escHtml(sceneSummary(sc)) + '</div><div class="sc-sub">' +
        escHtml(sc.type || "text") + " · " + dur.toFixed(1) + "s" +
        (sc.narration ? " · 旁白" : "") + '</div></div>' +
        '<div class="sc-mv"><button data-mv="-1" data-i="' + i +
        '" title="前移">▲</button><button data-mv="1" data-i="' + i +
        '" title="后移">▼</button></div></div>';
  }
  if (!scenes.length) html = '<div class="empty">spec 中还没有场景</div>';
  list.innerHTML = html;
  var cards = list.querySelectorAll(".sc-card");
  for (var c = 0; c < cards.length; c++) {
    cards[c].onclick = (function (idx) {
      return function (ev) {
        if (ev.target.getAttribute("data-mv")) return;
        selectScene(idx, true);
      };
    })(parseInt(cards[c].getAttribute("data-i"), 10));
  }
  var mv = list.querySelectorAll("[data-mv]");
  for (var m = 0; m < mv.length; m++) {
    mv[m].onclick = function (ev) {
      ev.stopPropagation();
      reorder(parseInt(this.getAttribute("data-i"), 10),
              parseInt(this.getAttribute("data-mv"), 10));
    };
  }
}
export function selectScene(idx, doSeek) {
  selIdx = idx;
  renderSceneList(); renderTimeline(); renderInspector();
  var w = $("pv").contentWindow;
  if (w) w.postMessage({ mcStudioHighlight: idx }, location.origin);
  if (doSeek && state.scenes[idx]) seekTo(state.scenes[idx].start + 0.01);
  // 选中后把片段滚入视图（不改变缩放），方便定位被压窄的短素材。
  var sc = $("tl-scroll");
  if (sc && state.scenes[idx]) {
    var s = state.scenes[idx];
    var left = HEAD_W + s.start * tlPPS;
    var right = left + Math.max(2, s.dur) * tlPPS;
    if (left < sc.scrollLeft + HEAD_W + 8)
      sc.scrollLeft = Math.max(0, left - HEAD_W - 24);
    else if (right > sc.scrollLeft + sc.clientWidth - 8)
      sc.scrollLeft = right - sc.clientWidth + 24;
  }
}
