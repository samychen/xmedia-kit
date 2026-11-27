import { $ } from "../shared/dom.js";
import { askAdd, askContextSnapshot, askMarkBackendPicked, askModeLabel, askOpen,
         askRun, askSend } from "./ask.js";
import { impOpen } from "../panels/import.js";
import { refresh } from "../panels/status.js";

/* ---------------- Pending edits 暂存队列 ---------------- */
/* 多次编辑攒一批一次应用（对齐 HF Pending video edits）：逐条走本地
   规则引擎（顺序应用、任一条失败即停并保留余项），全部结束后统一
   refresh 一次——预览/时间轴/属性面板只重组一次。 */
export var pendingEdits = [], pendingBusy = false;
export function pendingRender() {
  var box = $("ask-pending"), rows = $("ask-pending-rows");
  box.classList.toggle("has", pendingEdits.length > 0);
  $("ask-pending-n").textContent = pendingEdits.length;
  rows.replaceChildren();
  pendingEdits.forEach(function (t, i) {
    var row = document.createElement("div");
    row.className = "ask-pend-row";
    var n = document.createElement("span");
    n.className = "n"; n.textContent = (i + 1) + ".";
    var tx = document.createElement("span");
    tx.className = "t"; tx.textContent = t; tx.title = t;
    var del = document.createElement("button");
    del.type = "button"; del.textContent = "×";
    del.title = "移除这条"; del.setAttribute("aria-label", "移除");
    del.onclick = function () {
      pendingEdits.splice(i, 1); pendingRender();
    };
    row.appendChild(n); row.appendChild(tx); row.appendChild(del);
    rows.appendChild(row);
  });
  var apply = $("ask-pending-apply");
  apply.disabled = pendingEdits.length === 0 || pendingBusy;
  apply.textContent = pendingBusy ? "应用中…" :
      "批量应用" + (pendingEdits.length ? " " + pendingEdits.length : "");
  $("ask-pending-clear").disabled = pendingEdits.length === 0 || pendingBusy;
}
export function askStage(msg) {
  msg = String(msg || "").trim();
  if (!msg) return;
  pendingEdits.push(msg);
  $("ask-input").value = "";
  pendingRender();
  $("ask-input").focus();
}
export function askApplyPending() {
  if (pendingBusy || pendingEdits.length === 0) return;
  pendingBusy = true;
  askBusy = true;
  $("ask-send").disabled = true;
  pendingRender();
  var ctx = askContextSnapshot();
  var canBatchAi = askBackend === "agent" ||
      (state.agent && state.agent.llm && pendingEdits.length >= 2);
  if (canBatchAi) {
    var list = pendingEdits.slice();
    var merged = "请按以下待应用修改列表一次性调整当前项目：\n" +
        list.map(function (t, i) { return (i + 1) + ". " + t; }).join("\n");
    askAdd(merged, "user");
    pendingEdits = [];
    pendingRender();
    return askRun(merged, { context: ctx }).then(function (res) {
      if (res && res.error) pendingEdits = list;
    }).catch(function () {
      pendingEdits = list;
    }).then(function () {
      pendingBusy = false; askBusy = false;
      $("ask-send").disabled = false;
      pendingRender();
      return refresh(false).then(function () { askModeLabel(); });
    });
  }
  var done = 0, failed = -1;
  var next = function () {
    if (done >= pendingEdits.length || failed >= 0) {
      if (failed >= 0) {
        askAdd("批量应用在第 " + (failed + 1) + " 条失败，已应用 " +
               done + " 条；剩余 " + pendingEdits.length +
               " 条保留在队列", "bot", "err");
      } else {
        askAdd("批量应用完成：" + done + " 条", "bot", "ok");
        pendingEdits = [];
      }
      pendingBusy = false; askBusy = false;
      $("ask-send").disabled = false;
      pendingRender();
      return refresh(false).then(function () { askModeLabel(); });
    }
    var msg = pendingEdits.shift();
    askAdd(msg, "user");
    return askRun(msg, { context: ctx }).then(function () {
      ++done;
      return next();
    }).catch(function () {
      pendingEdits.unshift(msg);   // 失败条放回队首，保留现场
      failed = done;
      return next();
    });
  };
  return next();
}
$("ask-backend").onclick = function () {
  askMarkBackendPicked();
  askBackend = askBackend === "agent" ? "rule" : "agent";
  askModeLabel();
  askAdd(askBackend === "agent"
      ? "已切换到 agent 模式：本地 LLM 子进程自主改 spec/重跑 compose"
          + "（可在气泡下方随时点击停止）"
      : "已切换到规则引擎模式：预置指令与上下文即时生效（或走已配 API）", "bot");
};
$("ask-stage").onclick = function () { askStage($("ask-input").value); };
$("ask-pending-clear").onclick = function () {
  pendingEdits = []; pendingRender();
};
$("ask-pending-apply").onclick = askApplyPending;
$("ask-form").addEventListener("submit", function (ev) {
  ev.preventDefault();
  askSend($("ask-input").value);
});
[].forEach.call(document.querySelectorAll("#ask-quick button"), function (b) {
  b.onclick = function () {
    if (b.getAttribute("data-open") === "import") {
      askOpen(false); impOpen(true); return;
    }
    askSend(b.getAttribute("data-msg"));
  };
});
document.addEventListener("keydown", function (ev) {
  if ((ev.metaKey || ev.ctrlKey) && ev.code === "KeyK") {
    ev.preventDefault();
    // chat 常驻左栏：Ctrl/⌘+K = 聚焦对话输入（切到对话 tab）
    askOpen(true);
  } else if (ev.code === "Escape") {
    if ($("imp-panel").classList.contains("open")) {
      ev.preventDefault(); impOpen(false);
    }
  }
});
