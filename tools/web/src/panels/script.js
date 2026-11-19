import { $ } from "../shared/dom.js";
import { pendingEdits, pendingRender } from "../agent/pending.js";
import { zoomStep } from "../preview/zoom.js";

import { scriptTokenize } from "../shared/tokenize.js";
/* ---------------- Script 逐词面板（对齐 HF Script） ---------------- */
/* captions.segments 词元化：英文按空格、中文按字符。选择是 UI 概念，
   应用单位是段文本——Cut/Fix 重组段文本 → pending edit「字幕段 N 文字
   改成 …」→ 规则引擎/agent 应用。音频不剪（TTS 由旁白文本生成）。 */
export var scriptTokens = [], scriptSel = [], scriptCutSet = {}, scriptAnchor = -1;
export function scriptBuild() {
  scriptTokens = []; scriptSel = []; scriptCutSet = {}; scriptAnchor = -1;
  var caps = (state.spec || {}).captions;
  var segs = caps && Array.isArray(caps.segments) ? caps.segments : [];
  segs.forEach(function (seg, si) {
    var dur = Math.max(0.001, (seg.end || 0) - (seg.start || 0));
    var toks = scriptTokenize(String(seg.text || ""));
    var weights = toks.map(function (t) { return t.length; });
    var sum = weights.reduce(function (a, b) { return a + b; }, 0) || 1;
    var acc = 0;
    toks.forEach(function (t, wi) {
      var frac = acc / sum; acc += weights[wi];
      var nextFrac = acc / sum;
      scriptTokens.push({
        seg: si, wi: wi, text: t,
        start: (seg.start || 0) + frac * dur,
        end: (seg.start || 0) + nextFrac * dur,
        key: si + ":" + wi
      });
    });
  });
  $("script-meta").textContent = segs.length
      ? segs.length + " 段字幕 · " + scriptTokens.length + " 词元" :
      "需要 captions.segments（先用「自动字幕」或导入 ASR）";
}
export function scriptRender() {
  scriptBuild();
  var box = $("script-words");
  box.replaceChildren();
  scriptTokens.forEach(function (tk, i) {
    var el = document.createElement("span");
    el.textContent = tk.text + " ";
    el.style.cursor = "pointer";
    el.dataset.i = String(i);
    if (scriptCutSet[tk.key]) {
      el.style.textDecoration = "line-through";
      el.style.color = "var(--dim)";
      el.style.opacity = "0.55";
    }
    if (scriptSel.indexOf(i) >= 0) {
      el.style.background = "rgba(109,139,255,.3)";
      el.style.borderRadius = "3px";
    }
    el.onclick = function (ev) {
      if (ev.shiftKey && scriptAnchor >= 0) {
        var a = Math.min(scriptAnchor, i), b = Math.max(scriptAnchor, i);
        scriptSel = [];
        for (var k = a; k <= b; k++) scriptSel.push(k);
      } else {
        scriptSel = [i];
        scriptAnchor = i;
      }
      scriptRenderSel();
    };
    box.appendChild(el);
  });
  scriptButtons();
}
export function scriptRenderSel() {
  var spans = $("script-words").children;
  for (var i = 0; i < spans.length; i++) {
    spans[i].style.background = scriptSel.indexOf(i) >= 0
        ? "rgba(109,139,255,.3)" : "";
  }
  scriptButtons();
}
export function scriptButtons() {
  var has = scriptSel.length > 0;
  var hasCut = scriptTokens.some(function (tk) { return scriptCutSet[tk.key]; });
  $("script-cut").disabled = !has;
  $("script-fix").disabled = scriptSel.length !== 1;
  $("script-restore").disabled = !hasCut;
}
export function scriptSegText(segIdx, dropKeys) {
  /* 段内词元重组文本：dropKeys 里的词剔除，其余按原顺序拼接。 */
  var parts = [];
  scriptTokens.forEach(function (tk) {
    if (tk.seg !== segIdx) return;
    if (dropKeys && dropKeys[tk.key]) return;
    parts.push(tk.text);
  });
  return parts.join("");
}
$("script-cut").onclick = function () {
  scriptSel.forEach(function (i) { scriptCutSet[scriptTokens[i].key] = true; });
  var segs = {};
  scriptSel.forEach(function (i) {
    segs[scriptTokens[i].seg] = true;
  });
  Object.keys(segs).forEach(function (si) {
    var newText = scriptSegText(parseInt(si, 10), scriptCutSet)
        .replace(/\s+/g, " ").trim();
    if (!newText) return;   // 整段删空不生成（避免空字幕段）
    pendingEdits.push("字幕段 " + (parseInt(si, 10) + 1) + " 文字改成 " + newText);
  });
  scriptSel = [];
  pendingRender();
  scriptRender();
};
$("script-restore").onclick = function () {
  scriptCutSet = {};
  scriptRender();
};
$("script-fix").onclick = function () {
  var i = scriptSel[0], tk = scriptTokens[i];
  if (!tk) return;
  var v = window.prompt("修正词（原：" + tk.text + "）", tk.text);
  if (v === null || v.trim() === "" || v === tk.text) return;
  scriptTokens[i].text = v.trim();
  var newText = scriptSegText(tk.seg, scriptCutSet).replace(/\s+/g, " ").trim();
  pendingEdits.push("字幕段 " + (tk.seg + 1) + " 文字改成 " + newText);
  scriptSel = [];
  pendingRender();
  scriptRender();
};
$("pvwrap").addEventListener("wheel", function (ev) {
  if (!(ev.ctrlKey || ev.metaKey)) return;
  ev.preventDefault();
  zoomStep(ev.deltaY < 0 ? 1 : -1);
}, { passive: false });
