/* ---------------- 场景模板库（templates/ 目录的可发现性） ----------------
 * 19+ 个场景模板此前只能手写 spec 用——这里变成可浏览、可一键插入的
 * 卡片网格。「插入」走 Ask AI 规则引擎指令（"加一个 <名> 模板场景"），
 * 与对话/待应用队列同一编辑链；占位 slot 在 Inspector 里填。
 */
import { api } from "../shared/api-client.js";
import { $, escHtml, log } from "../shared/dom.js";
import { refresh } from "./status.js";

function makeTplThumb(t) {
  var box = document.createElement("div");
  box.className = "tpl-thumb";
  if (!t.preview_html) return box;
  var ifr = document.createElement("iframe");
  ifr.className = "comp-mini-frame";
  ifr.setAttribute("tabindex", "-1");
  ifr.onload = function () {
    try {
      var idoc = ifr.contentDocument;
      if (!idoc || !idoc.head || !idoc.body) return;
      var st = idoc.createElement("style");
      st.textContent =
        "html,body{margin:0;padding:0;overflow:hidden;background:#0f1117;}" +
        "#scene-0{width:1920px;height:1080px;position:relative;transform:scale(0.142);transform-origin:top left;}" +
        "#scene-0 *{opacity:1!important;visibility:visible!important;}" +
        (t.preview_css || "");
      idoc.head.appendChild(st);
      var sc = idoc.createElement("div");
      sc.id = "scene-0";
      sc.className = "clip scene scene-custom";
      sc.innerHTML = t.preview_html;
      idoc.body.appendChild(sc);
    } catch (e) {
      if (e) return;
    }
  };
  box.appendChild(ifr);
  return box;
}

export function tplRender() {
  var list = $("tpl-list");
  if (!list) return;
  list.innerHTML = '<div class="sb-empty">加载模板清单…</div>';
  api("/api/templates").then(function (r) {
    var tpls = r.templates || [];
    $("tpl-count").textContent = tpls.length + " 个模板";
    list.replaceChildren();
    if (!tpls.length) {
      list.innerHTML = '<div class="sb-empty">templates/ 目录为空</div>';
      return;
    }
    tpls.forEach(function (t) {
      var card = document.createElement("div");
      card.className = "tpl-card";
      var head = document.createElement("div");
      head.className = "tpl-head";
      head.innerHTML = '<span class="tpl-name">' + escHtml(t.name) +
          "</span>";
      var desc = document.createElement("div");
      desc.className = "tpl-desc";
      desc.textContent = t.description || "";
      var slots = document.createElement("div");
      slots.className = "tpl-slots";
      slots.textContent = "slots: " + (t.slots || []).join(" · ");
      var ins = document.createElement("button");
      ins.className = "btn tpl-ins"; ins.type = "button";
      ins.textContent = "插入场景";
      ins.setAttribute("aria-label", "插入 " + t.name + " 模板场景");
      ins.onclick = function () {
        ins.disabled = true;
        api("/api/agent", { message: "加一个 " + t.name + " 模板场景" })
          .then(function (r) {
            ins.disabled = false;
            log(r.reply || "已插入", r.error ? "err" : "ok");
            return refresh(true);   // state 同步：分镜/时间轴/场景列表更新
          })
          .catch(function (e) {
            ins.disabled = false;
            log("插入失败：" + e.message, "err");
          });
      };
      card.appendChild(makeTplThumb(t));
      card.appendChild(head); card.appendChild(desc);
      card.appendChild(slots); card.appendChild(ins);
      list.appendChild(card);
    });
  }).catch(function (e) {
    list.innerHTML = '<div class="sb-empty">模板清单加载失败：' +
        escHtml(e.message) + "</div>";
  });
}
