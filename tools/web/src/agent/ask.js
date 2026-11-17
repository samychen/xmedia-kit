import { pollJob } from "../panels/export.js";
import { refresh } from "../panels/status.js";
import { api } from "../shared/api-client.js";
import { log, $ } from "../shared/dom.js";

export function askAdd(text, who, cls) {
  var box = $("ask-msgs"), el = document.createElement("div");
  el.className = "ask-msg " + who + (cls ? " " + cls : "");
  el.textContent = text;
  box.appendChild(el);
  box.scrollTop = box.scrollHeight;
  return el;
}
export function askModeLabel() {
  var m = state.agent || {};
  var b = $("ask-backend");
  if (askBackend === "agent") {
    $("ask-mode").textContent = m.cli ? "本地 agent：" + m.cli :
                                        "本地 agent（未检测到 CLI）";
    b.textContent = "→ 规则引擎";
  } else {
    $("ask-mode").textContent = m.cli ? "本地规则引擎 · agent 可用" :
                                        "本地规则引擎";
    b.textContent = "→ agent";
  }
  b.disabled = false;
}
export var askBackend = "rule";
export function askOpen(on) {
  $("ask-panel").classList.toggle("open", !!on);
  if (on) { askModeLabel(); $("ask-input").focus(); }
}
/* chat 主界面化：页面加载即展开对话面板；本机 agent CLI 可用时默认
   agent 后端（HF Studio 的 chat 主界面形态——对话驱动一切）。首次
   refresh() 完成后 askModeLabel 会按实际 CLI 状态再刷新一次标签。 */
(function askBoot() {
  if ((state.agent || {}).cli) {
    askBackend = "agent";
  }
  askOpen(true);
})();
export function askRun(msg) {
  /* 单条编辑请求：按当前后端走 /api/agent（规则引擎）或
     /api/agent/chat（本地 agent 子进程），并渲染回复/导入/ASR/导出
     任务，不做 refresh——调用方决定何时统一重组（批量结束时一次）。 */
  var pending = askAdd("处理中…", "bot");
  var url = askBackend === "agent" ? "/api/agent/chat" : "/api/agent";
  return api(url, { message: msg }).then(function (r) {
    pending.textContent = r.reply || "已完成";
    pending.className = "ask-msg bot " + (r.error ? "err" : "ok");
    if (!r.error && (r.spec || r.spec_updated)) {
      /* spec 变更轮：附 Undo 按钮（撤销该轮及其后的全部编辑）。 */
      var undo = document.createElement("button");
      undo.className = "ask-undo"; undo.type = "button";
      undo.textContent = "↩ 撤销最近一轮";
      undo.onclick = function () {
        undo.disabled = true;
        api("/api/project/undo",
            { run: Math.max(0, (state.runs || []).length - 1) })
          .then(function () {
            undo.textContent = "已撤销";
            log("已撤销该轮编辑（之后的轮次一并撤销）", "ok");
          })
          .catch(function (e) {
            undo.textContent = "撤销失败"; undo.disabled = false;
            log("撤销失败：" + e.message, "err");
          })
          .then(function () { return refresh(false); });
      };
      pending.appendChild(document.createElement("br"));
      pending.appendChild(undo);
    }
    log("Ask AI [" + (r.mode || "rule") + "]：" +
        (r.reply || "") + (r.error ? "（" + r.error + "）" : ""),
        r.error ? "err" : "ok");
    if (r.import && r.import.notes) askAdd(r.import.notes.join("\n"), "bot");
    if (r.asr) {
      askAdd(r.asr.ok ? "识别到 " + r.asr.segments + " 段字幕" :
             (r.asr.error || "ASR 未产出字幕"), "bot",
             r.asr.ok ? "ok" : "err");
      if (r.asr.notes && r.asr.notes.length)
        askAdd(r.asr.notes.join("\n"), "bot");
    }
    if (r.action === "bake" && typeof r.job === "number") {
      askAdd("导出任务 #" + r.job + " 已启动", "bot");
      pollJob(r.job).then(function (j) {
        askAdd(j.rc === 0 ? "导出完成：" + j.out :
               "导出失败 rc=" + j.rc + "：" + j.tail, "bot",
               j.rc === 0 ? "ok" : "err");
      });
    }
    return r;
  }).catch(function (e) {
    pending.textContent = "请求失败：" + e.message;
    pending.className = "ask-msg bot err";
    log("Ask AI 失败：" + e.message, "err");
    throw e;
  });
}
export function askSend(msg) {
  msg = String(msg || "").trim();
  if (!msg || askBusy) return;
  askAdd(msg, "user");
  $("ask-input").value = "";
  askBusy = true;
  $("ask-send").disabled = true;
  askRun(msg).then(function () {
    return refresh(false);   // 服务端已写回 spec 并重组
  }).then(function () {
    askModeLabel();
  }).catch(function () {
    /* askRun 已渲染错误气泡；此处只需复位。 */
  }).then(function () {
    askBusy = false;
    $("ask-send").disabled = false;
    $("ask-input").focus();
  });
}
