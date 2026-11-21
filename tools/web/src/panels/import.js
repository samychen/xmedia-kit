import { refresh } from "./status.js";
import { api } from "../shared/api-client.js";
import { log, $ } from "../shared/dom.js";

/* ---------------- 导入本地素材（按钮 → 目录 / 文件 → /api/import） ---------------- */
/* 服务端持有 ffprobe / ASR / spec 写回能力。两条链路：
   ① 本机目录：直接传路径，服务端零拷贝读取（大素材首选）；
   ② 选择/拖入文件：multipart 上传到 spec 目录的 imported/ 批次目录。
   两条都汇合到 /api/import，成功后走 refresh() 统一刷新预览与时间轴。 */
export var impMode = "dir", impPicked = [], impBusy = false;
export function impSetStatus(text, cls) {
  var el = $("imp-status");
  el.textContent = text || "";
  el.className = (text ? "show " : "") + (cls || "");
}
export function impOpen(on) {
  var panel = $("imp-panel");
  if (on) {
    if (!panel.open) {
      try { panel.showModal();   // 模态：焦点圈定 + Esc + backdrop
      } catch (e) { panel.classList.add("fallback-open"); }  // 老 Safari 无 dialog API
    }
    panel.classList.add("open");
  } else {
    panel.classList.remove("open");
    if (panel.open) panel.close();
  }
  if (!on) return;
  var st = state.import || {};
  if (!$("imp-dir").value && st.last_dir) $("imp-dir").value = st.last_dir;
  $("imp-undo").disabled = !st.can_undo;
  $("imp-undo").title = st.can_undo
    ? "把时间轴恢复到上一次导入之前（" + (st.undo_label || "") + "）"
    : "暂无可撤销的导入";
  $("imp-foot-hint").textContent = st.spec_dir
    ? "默认只保留本次导入的素材；勾选「追加到当前时间轴」则接在末尾。spec 目录：" +
      st.spec_dir
    : "默认只保留本次导入的素材（清空时间轴重来）；勾选「追加到当前时间轴」则接在末尾";
  setTimeout(function () {
    try { (impMode === "dir" ? $("imp-dir") : $("imp-drop")).focus(); }
    catch (e) { /* 焦点失败不影响导入 */ }
  }, 0);
}
export function impTab(mode) {
  impMode = mode;
  $("imp-tab-dir").classList.toggle("on", mode === "dir");
  $("imp-tab-files").classList.toggle("on", mode === "files");
  $("imp-pane-dir").style.display = mode === "dir" ? "" : "none";
  $("imp-pane-files").style.display = mode === "files" ? "" : "none";
}
export function impClearFiles() {
  impPicked = [];
  var inp = $("imp-picker");
  if (inp) { try { inp.value = ""; } catch (e) { /* 部分浏览器只读 */ } }
  impRenderFiles();
}
export function impRenderFiles() {
  var box = $("imp-files");
  box.replaceChildren();
  impPicked.forEach(function (f, i) {
    var c = document.createElement("span");
    c.className = "imp-chip";
    var label = document.createElement("span");
    label.textContent = f.name + " · " + (f.size / 1048576).toFixed(1) + " MB";
    var x = document.createElement("button");
    x.type = "button";
    x.className = "x";
    x.textContent = "×";
    x.title = "从待导入列表移除（避免把上次选的素材再传一遍）";
    x.onclick = function () {
      impPicked.splice(i, 1);
      impRenderFiles();
      if (!impPicked.length) impSetStatus("");
    };
    c.appendChild(label);
    c.appendChild(x);
    box.appendChild(c);
  });
}
export function impAddFiles(list) {
  var seen = {};
  impPicked.forEach(function (f) { seen[f.name] = 1; });
  Array.prototype.slice.call(list || []).forEach(function (f) {
    if (!seen[f.name]) { impPicked.push(f); seen[f.name] = 1; }
  });
  impRenderFiles();
  if (impPicked.length) impTab("files");
}
export function impUpload() {
  var fd = new FormData();
  impPicked.forEach(function (f) { fd.append("file", f, f.name); });
  return fetch("/api/import/upload", { method: "POST", body: fd })
    .then(function (r) {
      return r.json().then(function (j) {
        if (j.error) throw new Error(j.error);
        return j;
      });
    });
}
export function impRun() {
  if (impBusy) return;
  var append = $("imp-append").checked;
  var opts = {
    format: $("imp-format").value,
    asr: $("imp-asr").checked,
    endcard: $("imp-endcard").checked,
    sfx: $("imp-sfx").checked,
    append: append,
    replace: !append,
    captions: $("imp-captions").value.trim()
  };
  var chain;
  if (impMode === "files") {
    if (!impPicked.length) {
      impSetStatus("请先选择或拖入音视频素材", "err"); return;
    }
    impSetStatus("正在接收 " + impPicked.length + " 个文件…");
    chain = impUpload().then(function (up) {
      opts.dir = up.dir;
      impSetStatus("已接收 " + up.count + " 个文件，ffprobe 正在读取时长…");
      return up;
    });
  } else {
    var dir = $("imp-dir").value.trim();
    if (!dir) { impSetStatus("请填写素材目录或单个文件路径（支持 ~ 展开）", "err"); return; }
    opts.dir = dir;
    chain = Promise.resolve(null);
  }
  impBusy = true;
  $("imp-go").disabled = true;
  $("imp-go").textContent = "导入中…";
  chain.then(function () { return api("/api/import", opts); })
    .then(function (r) {
      var c = r.composed || {};
      var lines = ["导入成功：" + r.dir];
      if (c.duration != null) {
        lines.push(c.duration.toFixed(2) + "s · " +
                   (c.scenes || []).length + " 个场景 · " +
                   (c.ok ? "契约校验通过" : c.errors + " 个校验错误"));
      }
      (r.notes || []).forEach(function (n) { lines.push("· " + n); });
      impSetStatus(lines.join("\n"), c.ok === false ? "err" : "ok");
      log("素材导入：" + r.dir + "（" + (c.scenes || []).length + " 场景，" +
          (r.mode === "append" ? "追加" : "替换") + "）", "ok");
      $("imp-undo").disabled = !r.can_undo;
      // 清空待导入列表：否则下次导入会把这次选过的文件再传一遍，
      // 时间轴前面就会多出上一次的素材。
      impClearFiles();
      return refresh(false);   // 服务端已写回 spec 并重组
    })
    .catch(function (e) {
      impSetStatus("导入失败：" + e.message, "err");
      log("素材导入失败：" + e.message, "err");
    })
    .then(function () {
      impBusy = false;
      $("imp-go").disabled = false;
      $("imp-go").textContent = "导入到时间轴";
    });
}
export function impUndo() {
  if (impBusy) return;
  impBusy = true;
  $("imp-undo").disabled = true;
  api("/api/import/undo", {}).then(function (r) {
    impSetStatus("已撤销导入，时间轴恢复为 " + (r.restored || "上一次导入前") +
                 " · " + ((r.composed || {}).scenes || []).length + " 个场景", "ok");
    log("已撤销上一次导入", "ok");
    $("imp-undo").disabled = true;
    return refresh(false);
  }).catch(function (e) {
    impSetStatus("撤销失败：" + e.message, "err");
    $("imp-undo").disabled = !(state.import || {}).can_undo;
  }).then(function () {
    impBusy = false;
  });
}
$("btn-import").onclick = function () { impOpen(true); };
$("imp-undo").onclick = impUndo;
$("imp-x").onclick = $("imp-cancel").onclick = function () { impOpen(false); };
/* backdrop 点击关闭（点击落在 dialog 元素自身 = ::backdrop 区域）；
   Esc 由模态 cancel→close 原生链路承担，这里同步 class。 */
$("imp-panel").addEventListener("click", function (ev) {
  if (ev.target === $("imp-panel")) impOpen(false);
});
$("imp-panel").addEventListener("close", function () {
  $("imp-panel").classList.remove("open");
});
$("imp-tab-dir").onclick = function () { impTab("dir"); };
$("imp-tab-files").onclick = function () { impTab("files"); };
$("imp-last").onclick = function () {
  var d = (state.import || {}).last_dir;
  if (d) { $("imp-dir").value = d; impTab("dir"); }
  else impSetStatus("本次会话还没有导入过目录", "err");
};
$("imp-specdir").onclick = function () {
  var d = (state.import || {}).spec_dir;
  if (d) { $("imp-dir").value = d; impTab("dir"); }
};
$("imp-dir").addEventListener("keydown", function (ev) {
  if (ev.isComposing || ev.keyCode === 229) return;  // CJK 组合中不触发
  if (ev.key === "Enter") { ev.preventDefault(); impRun(); }
});
$("imp-drop").onclick = function () { $("imp-picker").click(); };
$("imp-drop").addEventListener("keydown", function (ev) {
  if (ev.key === "Enter" || ev.key === " ") {
    ev.preventDefault(); $("imp-picker").click();
  }
});
$("imp-picker").addEventListener("change", function (ev) {
  impAddFiles(ev.target.files);
  ev.target.value = "";
});
["dragenter", "dragover"].forEach(function (t) {
  $("imp-drop").addEventListener(t, function (ev) {
    ev.preventDefault(); $("imp-drop").classList.add("over");
  });
});
["dragleave", "drop"].forEach(function (t) {
  $("imp-drop").addEventListener(t, function (ev) {
    ev.preventDefault(); $("imp-drop").classList.remove("over");
  });
});
$("imp-drop").addEventListener("drop", function (ev) {
  impAddFiles(ev.dataTransfer.files);
});
/* 页面任意位置拖入素材也能直接打开导入面板（避免浏览器导航到文件）。 */
document.addEventListener("dragover", function (ev) { ev.preventDefault(); });
document.addEventListener("drop", function (ev) {
  if ($("imp-panel").classList.contains("open")) return;
  if (!ev.dataTransfer || !ev.dataTransfer.files.length) return;
  ev.preventDefault();
  impOpen(true);
  impAddFiles(ev.dataTransfer.files);
});
$("imp-go").onclick = impRun;
