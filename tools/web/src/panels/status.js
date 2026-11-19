import { expOpen } from "./export.js";
import { renderInspector } from "./inspector.js";
import { renderSceneList } from "./scenes.js";
import { stopPhAnim } from "../preview/playhead.js";
import { fitPreview } from "../preview/zoom.js";
import { api } from "../shared/api-client.js";
import { log, $ } from "../shared/dom.js";
import { fps } from "../shared/time.js";
import { rangeClamp, rangeOpen } from "../timeline/range.js";
import { renderTimeline } from "../timeline/render.js";
import { storyboardIsOpen, storyboardRender } from "./storyboard.js";

/* ---------------- 校验 / 状态 ---------------- */
export function renderFindings(f) {
  var el = $("findings"); el.replaceChildren();
  // 只展示错误与警告：契约校验的 info 项是"已确认合规"的噪音，默认不占
  // 预览区高度；状态徽章已表达"校验通过"。
  (f || []).filter(function (x) {
    return x.level === "err" || x.level === "warn" || x.level === "error";
  }).forEach(function (x) {
    var d = document.createElement("div");
    d.className = "f " + x.level;
    d.textContent = "[" + x.level + "] " + x.message;
    el.appendChild(d);
  });
  var errors = (state.errors || 0);
  var badge = $("status");
  badge.className = "status " + (errors ? "bad" : "ok");
  badge.textContent = errors ? errors + " 个校验错误" : "契约校验通过";
}
export function refresh(keepSpec) {
  return api("/api/state").then(function (s) {
    s.spec = s.spec || {};
    state = s;
    // 重组后总长可能变化：把选区夹回 [0, duration]，完全越界则清空。
    if (tlRange) {
      tlRange = rangeClamp(tlRange);
      if (!rangeOpen()) tlRange = null;
    }
    // 只读预览（启动未带 --spec）无法写回，导入入口直接禁用并说明原因。
    $("btn-import").disabled = !s.spec_path;
    $("btn-import").title = s.spec_path
      ? "导入本地素材：本机目录 / 选择文件"
      : "只读预览：启动时需 --spec <path> 才能导入并写回";
    // 默认选中第一个场景，避免属性面板空着（Remotion 式打开即可编辑）。
    if (selIdx < 0 && s.spec.scenes && s.spec.scenes.length) selIdx = 0;
    if (selIdx >= (s.spec.scenes || []).length) selIdx = -1;
    if (!keepSpec) $("spec").value = JSON.stringify(s.spec, null, 2);
    $("sfx").checked = !(s.spec.sfx && s.spec.sfx.enabled === false);
    $("fileinfo").textContent = (s.spec_path || "未绑定 spec").split("/").pop();
    var comp = s.spec.composition || {};
    $("comp-name").textContent = comp.id || "composition";
    $("comp-meta").textContent = s.width + " × " + s.height + " · " +
        (comp.fps || 30) + "fps · " + (s.duration || 0).toFixed(1) + "s · " +
        ((s.spec.scenes || []).length) + " 场景";
    $("dims").textContent = s.width + "×" + s.height + " · " +
        (comp.fps || 30) + "fps";
    fitPreview(); renderSceneList(); renderTimeline(); renderInspector();
    if (storyboardIsOpen()) storyboardRender();
    renderFindings(s.findings);
    pendingSeek = lastTime;
    pendingApplied = false;
    // iframe 重载期间没有状态上报，先收住合成层过渡，避免游标跑向终点。
    if (phAnim) stopPhAnim(phVisual);
    playing = false;
    $("pv").src = "/preview?embed=1&t=" + Date.now();
  });
}
export function recompose() {
  var spec;
  try { spec = JSON.parse($("spec").value); }
  catch (e) { log("spec JSON 解析失败：" + e.message, "err"); return; }
  spec.sfx = { enabled: $("sfx").checked };
  api("/api/recompose", { spec: spec, no_sfx: !$("sfx").checked })
    .then(function (r) {
      return refresh(true).then(function () {
        log((r.ok ? "重组成功 " : "重组完成（含校验错误）") +
            r.duration.toFixed(1) + "s / " + r.scenes.length +
            " 场景 / sha " + r.sha, r.ok ? "ok" : "err");
        if (!r.ok) log("hf_validate 未过 0 errors——预览可用，导出会被拒绝",
                       "err");
      });
    })
    .catch(function (e) { log("重组失败：" + e.message, "err"); });
}
$("btn-recompose").onclick = recompose;
$("sfx").addEventListener("change", recompose);
$("btn-tts").onclick = function () {
  var spec;
  try { spec = JSON.parse($("spec").value); }
  catch (e) { log("spec JSON 解析失败：" + e.message, "err"); return; }
  $("btn-tts").disabled = true;
  api("/api/tts", { spec: spec }).then(function (r) {
    $("btn-tts").disabled = false;
    $("spec").value = JSON.stringify(r.spec, null, 2);
    log("旁白生成完成：" + r.generated + " 段（voice/ 目录）", "ok");
    recompose();
  }).catch(function (e) {
    $("btn-tts").disabled = false;
    log("TTS 失败：" + e.message, "err");
  });
};
$("btn-bake").onclick = function () {
  expOpen(true);
};
