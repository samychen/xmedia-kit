/* ---------------- 项目导航面板（HF 左栏的 Projects 形态） ----------------
 * 左栏第一 tab：项目列表（真帧缩略图卡）+ 新建入口 + 灵感库。
 * 编辑器内不离开即可切换项目（location.href 跳转，docking 重播）。
 * 灵感库 = 内置示例 spec 清单（/api/inspirations），一键「从灵感新建」
 * → /api/project/create（arc/message 来自灵感定义）。 */
import { api } from "../shared/api-client.js";
import { $, escHtml, log } from "../shared/dom.js";

export function projectsRender() {
  var view = $("projects-view");
  if (!view) return;
  view.innerHTML = '<div class="sb-empty">加载项目列表…</div>';
  api("/api/projects").then(function (r) {
    renderList(view, r.projects || []);
  }).catch(function (e) {
    view.innerHTML = '<div class="sb-empty">项目列表加载失败：' +
        escHtml(e.message) + "</div>";
  });
}

function renderList(view, projects) {
  view.replaceChildren();

  /* 新建入口（HF 的 "+ New video"）→ 跳 Home 完整向导 */
  var mkNew = document.createElement("a");
  mkNew.className = "pj-new";
  mkNew.href = "/?new=1";
  mkNew.innerHTML = "<b>＋ New video</b><i>做个东西——三步向导</i>";
  view.appendChild(mkNew);

  /* 灵感库（Inspirations）：示例 spec 一键新建 */
  var insp = document.createElement("div");
  insp.className = "pj-insp";
  insp.innerHTML = '<div class="pj-sec">Inspirations</div>';
  var inspRow = document.createElement("div");
  inspRow.className = "pj-insp-row";
  inspRow.innerHTML = '<span class="sb-empty">加载灵感…</span>';
  insp.appendChild(inspRow);
  view.appendChild(insp);
  api("/api/inspirations").then(function (r) {
    inspRow.replaceChildren();
    (r.inspirations || []).forEach(function (it) {
      var b = document.createElement("button");
      b.className = "pj-insp-card";
      b.type = "button";
      b.innerHTML = "<b>" + escHtml(it.title) + "</b><i>" +
          escHtml(it.hint) + "</i>";
      b.onclick = function () {
        b.disabled = true;
        api("/api/project/create", {
          arc: it.arc, message: it.message,
          name: "insp-" + it.id + "-" + Date.now() % 100000
        }).then(function (r2) {
          location.href = r2.url + "?fresh=1";
        }).catch(function (e) {
          b.disabled = false;
          log("灵感创建失败：" + e.message, "err");
        });
      };
      inspRow.appendChild(b);
    });
  }).catch(function () {
    inspRow.innerHTML = '<span class="sb-empty">灵感库不可用</span>';
  });

  /* 项目列表（真帧缩略图卡，当前项目高亮） */
  var sec = document.createElement("div");
  sec.className = "pj-sec";
  sec.textContent = "Projects " + projects.length;
  view.appendChild(sec);
  var grid = document.createElement("div");
  grid.className = "pj-nav";
  if (!projects.length) {
    grid.innerHTML = '<div class="sb-empty">还没有项目——上方从灵感或向导开始</div>';
  }
  var cur = (location.pathname.match(/\/project\/([\w-]+)/) || [])[1] || "";
  projects.forEach(function (p) {
    var card = document.createElement("a");
    card.className = "pj-nav-card" + (p.name === cur ? " cur" : "");
    card.href = "/project/" + p.name;
    card.innerHTML =
      '<div class="pj-thumb"><img src="/api/project-thumb/' +
      encodeURIComponent(p.name) + '" alt="" ' +
      'onerror="this.classList.add(\'miss\')"></div>' +
      '<div class="pj-nname">' + escHtml(p.name) + "</div>" +
      '<div class="pj-nmeta">' + p.scenes + " 场景 · " +
      escHtml(p.updated) + "</div>";
    grid.appendChild(card);
  });
  view.appendChild(grid);
}
