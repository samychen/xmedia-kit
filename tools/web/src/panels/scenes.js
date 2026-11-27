import { renderInspector } from "./inspector.js";
import { recompose } from "./status.js";
import { seekTo } from "../preview/driver.js";
import { escHtml, log, $ } from "../shared/dom.js";
import { HEAD_W } from "../shared/state.js";
import { renderTimeline } from "../timeline/render.js";
import { reorder } from "../timeline/trim.js";

/* ---------------- 场景列表 / Inspector ---------------- */
export var TECH_KEYS = { narration: 1, narration_volume: 1, duration: 1, bg: 1,
                  text_color: 1, animation: 1, transition: 1, video: 1,
                  video_from: 1, image: 1, comment: 1, captions: 1, design: 1,
                  type: 1, sfx: 1, padding: 1 };
export var FIELD_DEFS = {
  title: [["眉标 Kicker", "kicker"], ["标题", "text"], ["副标题", "subtitle"]],
  text: [["眉标 Kicker", "kicker"], ["主文本", "text"],
         ["子项（每行一条）", "content", "textarea"]],
  quote: [["眉标 Kicker", "kicker"], ["引文", "quote"], ["署名", "author"]],
  list: [["眉标 Kicker", "kicker"], ["标题", "text"],
         ["条目（每行一条）", "items", "textarea"]],
  stats: [["眉标 Kicker", "kicker"], ["数值", "value"], ["说明", "description"]],
  comparison: [["眉标 Kicker", "kicker"], ["左列标题", "left"], ["右列标题", "right"],
               ["左列条目（每行一条）", "left_items", "textarea"],
               ["右列条目（每行一条）", "right_items", "textarea"]],
  outro: [["文本", "text"]],
  html: [["HTML 内容", "html", "textarea"], ["CSS 样式", "css", "textarea"]],
};
export var SCENE_DEFAULTS = {
  title: { type: "title", kicker: "OVERVIEW", text: "核心主题标题",
           subtitle: "一句简洁有力的副标题说明", duration: 3.5, animation: "fade_in_up" },
  text: { type: "text", kicker: "DETAILS", text: "核心观点阐述",
          content: [{ text: "补充要点一" }, { text: "补充要点二" }],
          duration: 3.5, animation: "fade_in" },
  stats: { type: "stats", kicker: "BY THE NUMBERS", value: "10×",
           description: "关键指标实现数量级跃升", duration: 3.0, animation: "pop_in" },
  list: { type: "list", kicker: "HIGHLIGHTS", text: "三大核心优势",
          items: ["高确定性渲染与导出", "一键网站品牌扫描映射", "零配置 AI 创作驱动"],
          duration: 4.0, animation: "slide_in_left" },
  comparison: { type: "comparison", kicker: "COMPARISON", text: "方案前后对比",
                left: "传统制作", right: "智能工作流",
                left_items: ["反复手动微调", "样式零散难复用"],
                right_items: ["确定性结构排版", "预设一键全局统一"],
                duration: 4.2, animation: "fade_in" },
  quote: { type: "quote", kicker: "PERSPECTIVE",
           quote: "让每一帧构图与节奏都有清晰表达。", author: "创作团队",
           duration: 3.5, animation: "fade_in" },
  outro: { type: "outro", text: "立即开始体验", duration: 2.8, animation: "fade_in" },
  html: { type: "html", bg: "#0F1117", duration: 4.0,
          html: "<div class=\"anim\" style=\"display:flex;flex-direction:column;align-items:center;justify-content:center;height:100%;color:#fff;font-family:sans-serif\"><h1 style=\"font-size:72px;margin:0\">自由 HTML 场景</h1><p style=\"font-size:28px;opacity:.75\">支持任意 Flex/Grid/SVG 与 CSS 动效</p></div>",
          css: "", animation: "fade_in" },
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
  if (!list) return;
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
export function renderPresetSelect() {
  var sel = $("sc-preset-sel");
  if (!sel) return;
  var presets = state.presets || [];
  var cur = (state.spec && typeof state.spec.design === "string") ? state.spec.design : "";
  var html = '<option value="">默认风格（无预设）</option>';
  for (var i = 0; i < presets.length; i++) {
    var p = presets[i];
    var title = (p.description || "").split("：")[0] || p.name;
    html += '<option value="' + escHtml(p.name) + '">' +
        escHtml(p.name + " · " + title) + "</option>";
  }
  if (cur && !presets.some(function (x) { return x.name === cur; })) {
    html += '<option value="' + escHtml(cur) + '">' + escHtml(cur) + "</option>";
  }
  sel.innerHTML = html;
  sel.value = cur;
}
export function addScene(typ) {
  if (!state.spec || !Array.isArray(state.spec.scenes)) return;
  var tpl = SCENE_DEFAULTS[typ] || SCENE_DEFAULTS.text;
  var sc = structuredClone(tpl);
  var presets = state.presets || [];
  var dname = typeof state.spec.design === "string" ? state.spec.design : "";
  var hit = presets.find(function (p) { return p.name === dname; });
  if (hit && hit.bg) sc.bg = hit.bg;
  else if (state.spec.background) sc.bg = state.spec.background;
  if (hit && hit.ink) sc.text_color = hit.ink;
  var pos = selIdx >= 0 ? Math.min(state.spec.scenes.length, selIdx + 1)
                        : state.spec.scenes.length;
  state.spec.scenes.splice(pos, 0, sc);
  selIdx = pos;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("已新增场景 scene-" + pos + "（" + typ + "），重组合成…");
  recompose();
}
var presetSel = $("sc-preset-sel");
if (presetSel) {
  presetSel.onchange = function () {
    if (!state.spec) return;
    var val = presetSel.value;
    if (!val) {
      delete state.spec.design;
    } else {
      state.spec.design = val;
      var hit = (state.presets || []).find(function (p) { return p.name === val; });
      if (hit && hit.bg) state.spec.background = hit.bg;
      var scenes = state.spec.scenes || [];
      for (var i = 0; i < scenes.length; i++) {
        if (hit && hit.bg) scenes[i].bg = hit.bg;
        if (hit && hit.ink) scenes[i].text_color = hit.ink;
      }
    }
    $("spec").value = JSON.stringify(state.spec, null, 2);
    log("切换视觉预设 → " + (val || "默认") + "，重组合成…");
    recompose();
  };
}
var addBtn = $("sc-add-btn");
if (addBtn) {
  addBtn.onclick = function () {
    var tp = $("sc-add-type") ? $("sc-add-type").value : "text";
    addScene(tp);
  };
}
