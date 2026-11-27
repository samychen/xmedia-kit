import { pollJob } from "../panels/export.js";
import { refresh } from "../panels/status.js";
import { api } from "../shared/api-client.js";
import { log, $, escHtml } from "../shared/dom.js";
import { watch } from "../shared/store.js";

let activeJobId = null;
let drawBounds = null;
let ctxPinned = true;
let backendPickedByUser = false;
let historyLoaded = false;

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
  var modeEl = $("ask-mode");
  if (!b || !modeEl) return;
  if (askBackend === "agent") {
    modeEl.textContent = m.cli ? "本地 agent：" + m.cli :
                                 "本地 agent（未检测到 CLI）";
    b.textContent = "→ 规则引擎";
  } else {
    var via = m.llm ? ("大模型 " + (m.llm_model || "API")) : "规则引擎";
    modeEl.textContent = m.cli ? via + " · agent 可用" : via;
    b.textContent = "→ agent";
  }
  b.disabled = false;
  var cfgSt = $("ask-cfg-state");
  if (cfgSt) {
    cfgSt.textContent = m.llm
      ? ("已启用：" + (m.llm_model || "API"))
      : "未配置 API Key / Endpoint";
  }
}

export function askMarkBackendPicked() { backendPickedByUser = true; }

/* chat 常驻左栏「对话」面板：开 = 切到对话 tab；关 = 回场景 tab。 */
export function askOpen(on) {
  var tabs = { projects: "tab-projects", chat: "tab-chat",
               scenes: "tab-scenes", script: "tab-script",
               templates: "tab-templates",
               brand: "tab-brand", source: "tab-source" };
  var panels = { projects: "projects-panel", chat: "chat-panel",
                 scenes: "scenes-panel", script: "script-panel",
                 templates: "templates-panel",
                 brand: "brand-panel", source: "source-panel" };
  var name = on ? "chat" : "scenes";
  for (var k in tabs) {
    var t = $(tabs[k]);
    if (t) t.classList.toggle("active", k === name);
    var p = $(panels[k]);
    if (p) p.style.display = k === name ? "flex" : "none";
  }
  if (on) { askModeLabel(); $("ask-input").focus(); }
}

function renderCtxBar() {
  var bar = $("ask-ctx");
  if (!bar) return;
  bar.replaceChildren();
  var scenes = (state.spec && state.spec.scenes) || [];
  var items = [];
  if (ctxPinned && selIdx >= 0 && selIdx < scenes.length) {
    var sc = scenes[selIdx];
    var tm = Number(lastTime || 0).toFixed(1);
    items.push({
      label: "📌 第 " + (selIdx + 1) + " 镜 (" + (sc.type || "text") +
             ") · " + tm + "s",
      clear: function () { ctxPinned = false; renderCtxBar(); }
    });
  }
  if (drawBounds) {
    var b = drawBounds;
    items.push({
      label: "🎯 圈选画面 " + Math.round(b[0] * 100) + "%," +
             Math.round(b[1] * 100) + "%→" + Math.round(b[2] * 100) + "%," +
             Math.round(b[3] * 100) + "%",
      clear: function () { drawBounds = null; renderCtxBar(); }
    });
  }
  bar.classList.toggle("has", items.length > 0);
  items.forEach(function (it) {
    var chip = document.createElement("span");
    chip.className = "ask-ctx-chip";
    var txt = document.createElement("span");
    txt.textContent = it.label;
    var del = document.createElement("button");
    del.type = "button";
    del.textContent = "×";
    del.title = "取消附着";
    del.onclick = it.clear;
    chip.appendChild(txt);
    chip.appendChild(del);
    bar.appendChild(chip);
  });
}

export function askAttachDraw(bounds) {
  drawBounds = bounds;
  renderCtxBar();
  askOpen(true);
}

export function askContextSnapshot() {
  var scenes = (state.spec && state.spec.scenes) || [];
  var idx = (ctxPinned && selIdx >= 0 && selIdx < scenes.length) ? selIdx : -1;
  var ctx = { current_time: Number((lastTime || 0).toFixed(2)) };
  if (idx >= 0) ctx.scene_index = idx;
  if (drawBounds) ctx.draw_bounds = drawBounds;
  return ctx;
}

/* 极简 markdown → HTML（先转义再转标记，XSS 安全） */
function mdLite(text) {
  var s = escHtml(String(text || ""));
  s = s.replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>");
  s = s.replace(/`([^`]+)`/g, '<code style="background:var(--panel);'
      + 'padding:0 4px;border-radius:3px">$1</code>');
  s = s.replace(/^- (.+)$/gm, "· $1");
  return s.replace(/\n/g, "<br>");
}

function appendSteps(container, steps) {
  if (!steps || !steps.length) return;
  var wrap = document.createElement("div");
  wrap.className = "ask-steps";
  steps.slice(-6).forEach(function (st) {
    var row = document.createElement("div");
    row.className = "ask-step " + (st.status || "done");
    var icon = st.status === "running" ? "⏳" : "✓";
    row.textContent = icon + " " + (st.label || "");
    wrap.appendChild(row);
  });
  container.appendChild(wrap);
}

function appendDiff(container, diff) {
  if (!diff || !diff.length) return;
  var dif = document.createElement("div");
  dif.className = "ask-diff";
  dif.innerHTML = "<b>改动</b>" + diff.map(function (d) {
    return "<div>· " + escHtml(d) + "</div>";
  }).join("");
  container.appendChild(dif);
}

function appendUndo(container) {
  var undo = document.createElement("button");
  undo.className = "ask-undo";
  undo.type = "button";
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
        undo.textContent = "撤销失败";
        undo.disabled = false;
        log("撤销失败：" + e.message, "err");
      })
      .then(function () { return refresh(false); });
  };
  container.appendChild(undo);
}

function appendChoices(container, choices) {
  if (!choices || !choices.length) return;
  var wrap = document.createElement("div");
  wrap.className = "ask-choices";
  choices.slice(0, 4).forEach(function (ch) {
    var b = document.createElement("button");
    b.type = "button";
    b.className = "ask-choice";
    b.innerHTML = '<div class="ch-lbl">' + escHtml(ch.label || "") + "</div>" +
        (ch.desc ? '<div class="ch-desc">' + escHtml(ch.desc) + "</div>" : "");
    b.onclick = function () { askSend(ch.msg || ch.label || ""); };
    wrap.appendChild(b);
  });
  container.appendChild(wrap);
}

function appendSuggestions(container, sugs) {
  if (!sugs || !sugs.length) return;
  var row = document.createElement("div");
  row.className = "ask-sugs";
  sugs.slice(0, 4).forEach(function (sg) {
    var b = document.createElement("button");
    b.type = "button";
    b.className = "ask-sug";
    b.textContent = sg;
    b.onclick = function () { askSend(sg); };
    row.appendChild(b);
  });
  container.appendChild(row);
}

function renderBotBubble(el, opts) {
  el.replaceChildren();
  el.className = "ask-msg bot " + (opts.err ? "err" : "ok");
  if (opts.streaming) el.classList.add("streaming");
  if (opts.steps && opts.steps.length) appendSteps(el, opts.steps);
  var body = document.createElement("div");
  body.innerHTML = mdLite(opts.reply || "");
  el.appendChild(body);
  appendChoices(el, opts.choices);
  appendDiff(el, opts.diff);
  if (opts.canUndo) appendUndo(el);
  appendSuggestions(el, opts.suggestions);
  var box = $("ask-msgs");
  if (box) box.scrollTop = box.scrollHeight;
}

function setStopVisible(visible) {
  var sb = $("ask-stop");
  if (sb) sb.style.display = visible ? "" : "none";
}

function askRunAgentJob(msg, ctx) {
  var pending = askAdd("agent 启动中…", "bot");
  return api("/api/agent/chat", { message: msg, context: ctx })
    .then(function (r) {
      if (r.job === null || r.job === undefined) {
        renderBotBubble(pending, {
          reply: r.reply || "agent 不可用",
          err: true,
          suggestions: ["加一个标题卡", "全部转场改成 fade"]
        });
        return r;
      }
      activeJobId = r.job;
      setStopVisible(true);
      pending.classList.add("streaming");
      var poll = function () {
        return api("/api/agent/chat/status?id=" + r.job).then(function (s) {
          var tail = s.tail ? ("…" + String(s.tail).split("\n").slice(-3)
              .join("\n")) : "正在分析项目…";
          renderBotBubble(pending, {
            reply: tail,
            steps: s.steps,
            streaming: s.running
          });
          if (s.running) {
            return new Promise(function (res) {
              setTimeout(res, 650);
            }).then(poll);
          }
          return s;
        });
      };
      return poll().then(function (s) {
        activeJobId = null;
        setStopVisible(false);
        renderBotBubble(pending, {
          reply: s.reply || "（agent 无输出）",
          steps: s.steps,
          diff: s.diff,
          err: !!s.error,
          canUndo: !s.error && !!s.spec_updated
        });
        if (s.error) {
          log("agent [" + s.agent + "] 错误：" + s.error, "err");
        } else {
          log("agent [" + s.agent + "] 完成", "ok");
        }
        return s;
      }).catch(function (e) {
        activeJobId = null;
        setStopVisible(false);
        throw e;
      });
    });
}

export function askRun(msg, opts) {
  var ctx = (opts && opts.context) ? opts.context : askContextSnapshot();
  drawBounds = null;
  renderCtxBar();
  if (askBackend === "agent") {
    return askRunAgentJob(msg, ctx);
  }
  var pending = askAdd("处理中…", "bot");
  return api("/api/agent", { message: msg, context: ctx }).then(function (r) {
    if (String(r.reply || "").indexOf("我不确定这条指令") === 0 &&
        (state.agent || {}).cli) {
      pending.remove();
      log("规则引擎未命中，已转本地 agent 处理", "ok");
      return askRunAgentJob(msg, ctx);
    }
    renderBotBubble(pending, {
      reply: r.reply || "已完成",
      diff: r.diff,
      err: !!r.error,
      canUndo: !r.error && !!(r.spec || r.spec_updated),
      suggestions: r.suggestions,
      choices: r.choices
    });
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
    renderBotBubble(pending, { reply: "请求失败：" + e.message, err: true });
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
  askRun(msg).then(function (res) {
    var needRefresh = !res || res.spec || res.spec_updated ||
        res.action === "tts" || res.action === "import" ||
        res.action === "asr" || res.action === "reference" ||
        res.action === "recompose";
    return needRefresh ? refresh(false) : null;
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

function showWelcomeBubble() {
  var box = $("ask-msgs");
  if (!box || box.children.length) return;
  var el = askAdd("", "bot");
  renderBotBubble(el, {
    reply: "我是视频创作助手。你可以一句话生成整条视频、输入网址自动提取品牌风格，或选中任意镜头让我微调。",
    choices: [
      { label: "🚀 一句话 0-1 生成发布短片",
        desc: "自动规划 5 镜叙事与暗夜科技质感预设",
        msg: "帮我做一个 AI 效率产品介绍视频" },
      { label: "✨ 探索视觉预设与风格方向",
        desc: "在暖石、科技蓝、珊瑚红等 7 套预设中挑选灵感",
        msg: "我想做一个产品视频" },
    ],
    suggestions: ["换成 editorial-stone 风格", "全部转场改成 wipe", "生成旁白"]
  });
}

function loadChatHistoryOnce() {
  if (historyLoaded) return;
  historyLoaded = true;
  api("/api/agent/history").then(function (res) {
    var list = (res && res.history) || [];
    if (!list.length) {
      showWelcomeBubble();
      return;
    }
    var box = $("ask-msgs");
    if (box) box.replaceChildren();
    list.forEach(function (item) {
      if (item.user) askAdd(item.user, "user");
      if (item.assistant) {
        var el = askAdd("", "bot");
        renderBotBubble(el, {
          reply: item.assistant,
          diff: item.diff,
          steps: item.steps,
          suggestions: item.suggestions,
          choices: item.choices
        });
      }
    });
  }).catch(function () {
    showWelcomeBubble();
  });
}

function bindHeadControls() {
  var stopBtn = $("ask-stop");
  if (stopBtn) stopBtn.onclick = function () {
    if (activeJobId === null) return;
    stopBtn.disabled = true;
    api("/api/agent/chat/cancel", { id: activeJobId }).catch(function (e) {
      log("中止失败：" + e.message, "err");
    }).then(function () { stopBtn.disabled = false; });
  };
  var clrBtn = $("ask-clear-btn");
  if (clrBtn) clrBtn.onclick = function () {
    api("/api/agent/history/clear", {}).then(function () {
      var box = $("ask-msgs");
      if (box) box.replaceChildren();
      showWelcomeBubble();
    }).catch(function (e) {
      log("清空对话失败：" + e.message, "err");
    });
  };
  var cfgBtn = $("ask-cfg-btn"), cfgBox = $("ask-cfg");
  if (cfgBtn && cfgBox) cfgBtn.onclick = function () {
    var open = cfgBox.style.display === "none";
    cfgBox.style.display = open ? "flex" : "none";
    cfgBtn.classList.toggle("on", open);
    if (open) {
      api("/api/agent/config").then(function (c) {
        $("ask-cfg-ep").value = c.endpoint || "";
        $("ask-cfg-model").value = c.model || "gpt-4o-mini";
        $("ask-cfg-key").value = "";
      }).catch(function () {});
    }
  };
  var cfgClose = $("ask-cfg-close");
  if (cfgClose && cfgBox) cfgClose.onclick = function () {
    cfgBox.style.display = "none";
    if (cfgBtn) cfgBtn.classList.remove("on");
  };
  var cfgSave = $("ask-cfg-save");
  if (cfgSave) cfgSave.onclick = function () {
    var payload = {
      endpoint: $("ask-cfg-ep").value.trim(),
      model: $("ask-cfg-model").value.trim(),
      api_key: $("ask-cfg-key").value.trim()
    };
    api("/api/agent/config", payload).then(function (r) {
      if (cfgBox) cfgBox.style.display = "none";
      if (cfgBtn) cfgBtn.classList.remove("on");
      log(r.configured ? "已保存大模型接口配置（" + r.model + "）" : "已清空大模型接口配置", "ok");
      return refresh(false);
    }).catch(function (e) {
      log("保存配置失败：" + e.message, "err");
    });
  };
}

watch("selIdx", function () {
  ctxPinned = true;
  renderCtxBar();
});

watch("state", function (s) {
  if (!backendPickedByUser && s && s.agent && s.agent.cli) {
    askBackend = "agent";
  }
  askModeLabel();
  renderCtxBar();
  if (s && s.spec_path) loadChatHistoryOnce();
});

bindHeadControls();
showWelcomeBubble();
askOpen(true);
