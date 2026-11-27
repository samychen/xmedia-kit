import { expOpen } from "./export.js";
import { renderInspector } from "./inspector.js";
import { renderSceneList, renderPresetSelect } from "./scenes.js";
import { stopPhAnim } from "../preview/playhead.js";
import { fitPreview } from "../preview/zoom.js";
import { api } from "../shared/api-client.js";
import { escHtml, log, $ } from "../shared/dom.js";
import { fps } from "../shared/time.js";
import { rangeClamp, rangeOpen } from "../timeline/range.js";
import { renderTimeline } from "../timeline/render.js";
import { sbGridIsOn, sbGridRender } from "../preview/sbgrid.js";

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
    /* 面包屑项目名：spec 路径两级上级 = 项目目录名（Home 模式）。
       行内重命名（HF 顶栏铅笔）：点击变输入框，Enter/失焦提交。 */
    var pp = (s.spec_path || "").split("/");
    var cname = pp.length >= 2 ? pp[pp.length - 2] : "";
    if (!$("crumb-name").dataset.editing) {
      $("crumb-name").textContent = cname;
      $("crumb-name").dataset.proj = cname;
    }
    var comp = s.spec.composition || {};
    $("comp-name").textContent = comp.id || "composition";
    $("comp-meta").textContent = s.width + " × " + s.height + " · " +
        (comp.fps || 30) + "fps · " + (s.duration || 0).toFixed(1) + "s · " +
        ((s.spec.scenes || []).length) + " 场景";
    $("dims").textContent = s.width + "×" + s.height + " · " +
        (comp.fps || 30) + "fps";
    fitPreview(); renderPresetSelect(); renderSceneList(); renderTimeline(); renderInspector();
    var mediaDl = $("insp-media-list");
    if (mediaDl) {
      mediaDl.innerHTML = (s.media_files || []).map(function (m) {
        return '<option value="' + escHtml(m) + '"></option>';
      }).join("");
    }
    if (sbGridIsOn()) sbGridRender();
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

/* 项目名行内重命名（HF 顶栏铅笔）：点击面包屑项目名变输入框，
   Enter/失焦提交 → /api/project/rename → 跳新地址。 */
$("crumb-name").onclick = function () {
  if (this.dataset.editing) return;
  var old = this.dataset.proj || "";
  if (!old) return;
  this.dataset.editing = "1";
  this.textContent = "";
  var inp = document.createElement("input");
  inp.className = "crumb-edit"; inp.value = old;
  inp.maxLength = 40; inp.spellcheck = false;
  this.appendChild(inp);
  inp.focus(); inp.select();
  var done = (commit) => {
    if (!this.dataset.editing) return;
    delete this.dataset.editing;
    var nv = inp.value.trim();
    if (!commit || !nv || nv === old) {
      this.textContent = old;
      return;
    }
    api("/api/project/rename", { name: old, new_name: nv })
      .then(function (r) {
        log("已重命名 → " + r.name, "ok");
        location.href = r.url + "?fresh=1";
      })
      .catch((e) => {
        this.textContent = old;
        log("重命名失败：" + e.message, "err");
      });
  };
  inp.onkeydown = function (ev) {
    if (ev.key === "Enter") { ev.preventDefault(); done(true); }
    if (ev.key === "Escape") { done(false); }
  };
  inp.onblur = function () { done(true); };
};

export function refreshBrandChip() {
  var chip = $("ask-brand-chip");
  if (!chip) return;
  while (chip.options.length > 1) chip.remove(1);
  api("/api/brands").then(function (r) {
    (r.brands || []).forEach(function (b) {
      var o = document.createElement("option");
      o.value = b.name;
      o.textContent = "🏷 " + b.name + (b._default ? "（默认）" : "");
      chip.appendChild(o);
    });
  }).catch(function () {});
}

/* 品牌 chip（HF 输入区 "No brand kit" 同款）：/api/brands 填充下拉，
   选择即发「应用品牌」对话（走现有编辑链，消息在 chat 里可见）。 */
(function () {
  var chip = $("ask-brand-chip");
  if (!chip) return;
  chip.onchange = function () {
    var v = chip.value;
    if (!v) return;
    var form = document.getElementById("ask-form");
    var input = document.getElementById("ask-input");
    input.value = "应用品牌 " + v;
    if (typeof window.requestSubmit === "function" && form.requestSubmit) {
      form.requestSubmit(document.getElementById("ask-send"));
    } else {
      form.dispatchEvent(new Event("submit",
        { cancelable: true, bubbles: true }));
    }
    chip.value = "";   // 回到 No brand kit（品牌应用是一次性动作）
  };
  refreshBrandChip();
})();
