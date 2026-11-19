import { pollJob } from "../panels/export.js";
import { refresh } from "../panels/status.js";
import { api } from "../shared/api-client.js";
import { log, $, escHtml } from "../shared/dom.js";

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
/* chat 常驻左栏「对话」面板：开 = 切到对话 tab；关 = 回场景 tab。
   （HF 的 chat 左栏主界面形态——对话不是浮层，是主面板。） */
export function askOpen(on) {
  var tabs = { chat: "tab-chat", scenes: "tab-scenes",
               script: "tab-script", storyboard: "tab-storyboard",
               source: "tab-source" };
  var panels = { chat: "chat-panel", scenes: "scenes-panel",
                 script: "script-panel", storyboard: "sb-panel",
                 source: "source-panel" };
  var name = on ? "chat" : "scenes";
  for (var k in tabs) {
    $(tabs[k]).classList.toggle("active", k === name);
    var p = $(panels[k]);
    if (p) p.style.display = k === name ? "flex" : "none";
  }
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
/* 极简 markdown → HTML（先转义再转标记，XSS 安全）：
   **粗体** / `代码` / "- 列表" / 换行。 */
function mdLite(text) {
  var s = escHtml(String(text || ""));
  s = s.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>");
  s = s.replace(/`([^`]+)`/g, '<code style="background:var(--panel2);'
      + 'padding:0 4px;border-radius:3px">$1</code>');
  s = s.replace(/^- (.+)$/gm, "· $1");
  return s.replace(/\n/g, "<br>");
}

/* agent 模式（job 化流式）：POST 创建 job → 轮询 tail（实时显示
   agent 在做什么——HF 的过程可视化）→ done 渲染 markdown 回复 +
   改动明细卡。spec 被改时 refresh（分镜/预览同步）。 */
function askRunAgentJob(msg) {
  var pending = askAdd("agent 启动中…", "bot");
  return api("/api/agent/chat", { message: msg }).then(function (r) {
    if (r.job === null || r.job === undefined) {
      pending.textContent = r.reply || "agent 不可用";
      pending.className = "ask-msg bot err";
      return null;
    }
    pending.classList.add("streaming");
    var poll = function () {
      return api("/api/agent/chat/status?id=" + r.job).then(function (s) {
        if (s.tail) {
          var tail = String(s.tail).split("\n").slice(-3).join("\n");
          pending.innerHTML = mdLite("…" + tail);
        }
        if (s.running) {
          return new Promise(function (res) {
            setTimeout(res, 700);
          }).then(poll);
        }
        return s;
      });
    };
    return poll().then(function (s) {
      pending.innerHTML = mdLite(s.reply || "（agent 无输出）");
      pending.className = "ask-msg bot " +
          (s.error ? "err" : "ok");
      if (s.diff && s.diff.length) {
        var dif = document.createElement("div");
        dif.className = "ask-diff";
        dif.innerHTML = "<b>改动</b>" + s.diff.map(function (d) {
          return "<div>· " + escHtml(d) + "</div>";
        }).join("");
        pending.appendChild(dif);
      }
      if (s.error) {
        log("agent [" + s.agent + "] 错误：" + s.error, "err");
      } else {
        log("agent [" + s.agent + "] 完成", "ok");
      }
      return s;
    });
  });
}

export function askRun(msg) {
  /* 单条编辑请求：按当前后端走 /api/agent（规则引擎）或
     /api/agent/chat（本地 agent 子进程，job 化流式），并渲染回复/
     导入/ASR/导出任务，不做 refresh——调用方决定何时统一重组。
     规则引擎未命中（"我不确定…"）且 agent CLI 可用 → 自动转
     agent 处理（HF chat 主界面形态：任意话都有人接）。 */
  if (askBackend === "agent") {
    return askRunAgentJob(msg);
  }
  var pending = askAdd("处理中…", "bot");
  return api("/api/agent", { message: msg }).then(function (r) {
    if (String(r.reply || "").indexOf("我不确定这条指令") === 0 &&
        (state.agent || {}).cli) {
      pending.remove();
      log("规则引擎没听懂，已转 agent 处理", "ok");
      return askRunAgentJob(msg).then(function (s) {
        if (!s.error && (s.spec_updated || s.diff)) {
          return refresh(true);
        }
        return s;
      });
    }
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
