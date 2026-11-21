import { renderSceneList, sceneFields } from "./scenes.js";
import { recompose } from "./status.js";
import { escHtml, log, $ } from "../shared/dom.js";
import { api } from "../shared/api-client.js";
import { renderTimeline } from "../timeline/render.js";
import { sameSourceSeam } from "../timeline/trim.js";

export function renderInspector() {
  var sc = (selIdx >= 0 && state.spec.scenes &&
            state.spec.scenes[selIdx]) ? state.spec.scenes[selIdx] : null;
  $("insp-empty").style.display = sc ? "none" : "block";
  $("insp-body").style.display = sc ? "block" : "none";
  if (!sc) { $("insp-title").textContent = ""; return; }
  var locked = !!sc.narration;
  $("insp-title").textContent = "scene-" + selIdx + " · " + (sc.type || "text");
  var metaDur = (state.scenes[selIdx] && state.scenes[selIdx].dur) || 0;
  $("insp-dur").value = (locked ? metaDur : (sc.duration || metaDur)).toFixed(2);
  $("insp-dur").disabled = locked;
  $("insp-lock").textContent = locked ? "时长随旁白拟合，已锁定" : "";
  // 未显式写字段时回填 hf_compose 的默认值，避免色块显示成误导性的黑色。
  $("insp-bg").value = /^#[0-9a-fA-F]{6}$/.test(sc.bg || "") ?
      sc.bg : "#000000";
  $("insp-color").value = /^#[0-9a-fA-F]{6}$/.test(sc.text_color || "") ?
      sc.text_color : "#ffffff";
  $("insp-anim").value = sc.animation || "fade_in";
  $("insp-trans").value = sc.transition || "";
  /* 片段评论（HF Comment 同款）：元数据随 spec 保存，不进契约页 */
  $("insp-comment").value = sc.comment || "";
  $("insp-cmt-badge").textContent = sc.comment ? "· 已留" : "";
  var fields = sceneFields(sc);
  curFields = fields;
  // 视频场景暴露「源内起点」：切割产出的 video_from 在这里看得见、改得动
  // （改大 = 从素材更后面开始取画面/音轨）。
  var fromRow = sc.video
      ? '<div class="field"><span>源内起点</span><input id="insp-from" value="' +
        (Number(sc.video_from) || 0).toFixed(2) +
        '"' + (locked ? " disabled" : "") + '></div>'
      : "";
  $("insp-fields").innerHTML = fromRow + fields.map(function (f) {
    var v = sc[f[1]];
    var val = (f[1] === "content")
        ? (v || []).map(function (x) { return x.text || ""; }).join("\n")
        : (typeof v === "string" ? v
           : Array.isArray(v) ? v.join("\n") : (v == null ? "" : String(v)));
    var dis = (f[1] === "text" && locked) ? " disabled" : "";
    var label = '<div class="field"><span>' + escHtml(f[0]) + '</span>';
    if (f[2] === "textarea") {
      return label + '<textarea id="insp-f-' + f[1] + '" rows="3"' +
             dis + '>' + escHtml(val) + '</textarea></div>';
    }
    return label + '<input id="insp-f-' + f[1] + '" value="' +
           escHtml(val) + '"' + dis + '></div>';
  }).join("") + (locked
      ? '<div class="field" style="font-size:10.5px;color:var(--dim)">'
        + '🔒 该场景已生成旁白音频，文字锁定以保持音画同步——改文案请在 '
        + 'Ask AI 里说「把第 ' + (selIdx + 1) + ' 个场景文字改成 …」（会同步'
        + '刷新旁白指引），或点顶栏「生成旁白」重做音频</div>'
      : "");
}
export function applyInspector() {
  if (selIdx < 0 || !state.spec.scenes[selIdx]) return;
  var sc = state.spec.scenes[selIdx], locked = !!sc.narration;
  if (!locked) {
    sc.duration = Math.max(0.3, parseFloat($("insp-dur").value) || sc.duration);
  }
  sc.bg = $("insp-bg").value;
  sc.text_color = $("insp-color").value;
  sc.animation = $("insp-anim").value;
  var tr = $("insp-trans").value;
  if (tr) sc.transition = tr; else delete sc.transition;
  if (tr && tr !== "none") {
    // 转场要靠进入侧入场动画让位：animation 还是硬切的 none 时，新场景
    // 瞬间盖满，前一场景的出场动画全被遮住（看回来还是硬切）。
    // 窗宽由 hf_compose 决定（全局 transitions.duration 为 0 时，显式声明
    // 的转场按 0.5s 处理），所以这里不用改 spec 的全局段。
    if (sc.animation === "none") {
      sc.animation = "fade_in";
    }
  }
  if (sc.video && $("insp-from")) {
    var vf = Math.max(0, parseFloat($("insp-from").value) || 0);
    if (vf > 0) sc.video_from = Math.round(vf * 1000) / 1000;
    else delete sc.video_from;
  }
  for (var i = 0; i < curFields.length; i++) {
    var key = curFields[i][1], el = $("insp-f-" + key);
    if (!el) continue;
    var raw = el.value;
    if (key === "content") {
      sc.content = raw.split("\n").filter(function (l) { return l.trim(); })
                      .map(function (l) { return { text: l.trim() }; });
    } else if (key === "items" || key === "left_items" ||
               key === "right_items" || curFields[i][2] === "textarea") {
      sc[key] = raw.split("\n").filter(function (l) { return l.trim(); })
                   .map(function (l) { return l.trim(); });
    } else {
      sc[key] = raw.trim();
    }
  }
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("inspector → scene-" + selIdx + "，重组合成…");
  recompose();
}
$("insp-apply").onclick = applyInspector;
/* 转场 / 入场动画改成「选完即生效」，不再要求再点一次「应用到场景」：
   这两个下拉最常用来试转场，点完没反应会像是功能坏了（其它字段仍然
   显式应用，避免输入中途反复重组）。 */
$("insp-trans").onchange = function () {
  if (selIdx < 0) return;
  applyInspector();
  // 同一素材的连续两段：fade 的两边是几乎相同的画面，混合前后亮度一样，
  // 看起来就像「转场没反应」。这不是没生效，是素材本身没有差异。
  if ($("insp-trans").value === "fade" && sameSourceSeam(selIdx)) {
    log("提示：scene-" + (selIdx - 1) + " / scene-" + selIdx +
        " 是同一素材的连续画面，fade 交叉淡化在画面上看不出变化；" +
        "要可见效果请用 slide-left / slide-right / wipe", "warn");
  }
};
$("insp-anim").onchange = function () {
  if (selIdx < 0) return;
  applyInspector();
};
$("insp-dup").onclick = function () {
  if (selIdx < 0 || !state.spec.scenes[selIdx]) return;
  var copy = structuredClone(state.spec.scenes[selIdx]);
  state.spec.scenes.splice(selIdx + 1, 0, copy);
  selIdx += 1;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  log("已复制场景 → scene-" + selIdx + "，重组合成…");
  recompose();
};
$("insp-del").onclick = function () {
  if (selIdx < 0) return;
  state.spec.scenes.splice(selIdx, 1);
  selIdx = -1;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderSceneList(); renderTimeline(); renderInspector();
  log("场景已删除，重组合成…");
  recompose();
};

/* 评论保存（轻量 /api/spec-save：写回不重组——评论不进契约页） */
$("insp-cmt-save").onclick = function () {
  if (selIdx < 0 || !state.spec.scenes[selIdx]) return;
  var text = $("insp-comment").value.trim();
  if (text) state.spec.scenes[selIdx].comment = text;
  else delete state.spec.scenes[selIdx].comment;
  $("insp-cmt-badge").textContent = text ? "· 已留" : "";
  this.disabled = true;
  api("/api/spec-save", { spec: state.spec }).then(() => {
    this.disabled = false;
    log(r.errors ? "评论已保存（校验 " + r.errors + " errors）" : "评论已保存",
        r.errors ? "err" : "ok");
  }).catch((e) => {
    this.disabled = false;
    log("评论保存失败：" + e.message, "err");
  });
};
