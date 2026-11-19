/* ---------------- 品牌 kits（HF Brand kits 左栏形态） ----------------
 * 列表 + 扫描表单（Scan my site）+ kit 详情（Colors/Text styles/
 * Logo/组件预览卡）+ 应用到当前项目 / 设为默认（default pin）。
 * 扫描走 POST /api/brand/scan；应用复用 /api/agent「应用品牌 <名>」
 * 规则引擎指令（与模板插入同一编辑链）；默认走 /api/brand/default。 */
import { api } from "../shared/api-client.js";
import { $, escHtml, log } from "../shared/dom.js";
import { refresh } from "./status.js";

export function brandRender() {
  var view = $("brand-view");
  if (!view) return;
  view.innerHTML = '<div class="sb-empty">加载品牌 kits…</div>';
  api("/api/brands").then(function (r) {
    renderList(view, r.brands || []);
  }).catch(function (e) {
    view.innerHTML = '<div class="sb-empty">品牌清单加载失败：' +
        escHtml(e.message) + "</div>";
  });
}

function renderList(view, kits) {
  view.replaceChildren();

  /* 扫描表单（HF「New brand kit / Scan my site」形态） */
  var form = document.createElement("div");
  form.className = "brand-scan";
  var input = document.createElement("input");
  input.id = "brand-url"; input.type = "url";
  input.placeholder = "https://www.bilibili.com";
  input.setAttribute("aria-label", "网站 URL");
  var go = document.createElement("button");
  go.className = "btn brand-scan-go"; go.type = "button";
  go.textContent = "扫描生成 kit";
  go.onclick = function () {
    var url = (input.value || "").trim();
    if (!url || go.disabled) return;
    go.disabled = true; input.disabled = true;
    go.textContent = "扫描中…（抓页面/字体/logo）";
    api("/api/brand/scan", { url: url }).then(function (r) {
      log("品牌「" + r.name + "」已生成（" +
          ((r.brand || {}).palette || []).length + " 色）", "ok");
      brandRender();
    }).catch(function (e) {
      go.disabled = false; input.disabled = false;
      go.textContent = "扫描生成 kit";
      log("扫描失败：" + e.message, "err");
    });
  };
  form.appendChild(input);
  form.appendChild(go);
  view.appendChild(form);

  /* kit 卡片网格 */
  var grid = document.createElement("div");
  grid.className = "brand-grid";
  if (!kits.length) {
    var empty = document.createElement("div");
    empty.className = "sb-empty";
    empty.textContent = "还没有品牌 kit——上面贴个网址扫一个";
    grid.appendChild(empty);
  }
  kits.forEach(function (b) {
    var card = document.createElement("div");
    card.className = "brand-card";
    card.setAttribute("role", "button");
    card.setAttribute("tabindex", "0");
    var palette = b.palette || [];
    var colors = (b.colors || []).length ?
        (b.colors || []).map(function (c) { return c.hex; }) : palette;
    var host = String(b.source_url || "").replace(/^https?:\/\//, "");
    card.innerHTML =
      '<div class="brand-card-head">' +
      '<span class="brand-name">' + escHtml(b.name || "") + "</span>" +
      (b._default ? '<span class="brand-def">默认</span>' : "") +
      "</div>" +
      (b.logo_image ? '<img class="brand-logo" src="/api/brand-logo/' +
          encodeURIComponent(b.name) + '" alt="">' : "") +
      '<div class="brand-swatches">' +
      colors.slice(0, 6).map(function (c) {
        return '<i style="background:' + escHtml(c) + '"></i>';
      }).join("") + "</div>" +
      '<div class="brand-meta">' +
      (host ? escHtml(host) + " · " : "") +
      colors.length + " 色" +
      ((b.fonts || []).length ? " · " + escHtml(b.fonts[0]) : "") +
      "</div>";
    var open = function () { renderDetail(view, b); };
    card.onclick = open;
    card.onkeydown = function (ev) {
      if (ev.key === "Enter" || ev.key === " ") { ev.preventDefault(); open(); }
    };
    grid.appendChild(card);
  });
  view.appendChild(grid);
}

function renderDetail(view, b) {
  var name = b.name || "";
  var palette = b.palette || [];
  var colors = (b.colors || []).length ?
      (b.colors || []).map(function (c) { return c.hex; }) : palette;
  var ink = b.ink || b.text_color || "";
  var bg = b.background || "";
  var fonts = b.fonts || [];
  var host = String(b.source_url || "").replace(/^https?:\/\//, "");
  view.replaceChildren();

  /* 头：返回 + 名 + 来源 + 动作 */
  var head = document.createElement("div");
  head.className = "brand-detail-head";
  var back = document.createElement("button");
  back.className = "btn brand-back"; back.type = "button";
  back.textContent = "← 所有 kits";
  back.onclick = function () { brandRender(); };
  var title = document.createElement("div");
  title.className = "brand-detail-title";
  title.innerHTML = '<span class="brand-name-lg">' + escHtml(name) +
      "</span>" + (b._default ? '<span class="brand-def">默认</span>' : "") +
      (host ? '<span class="brand-src">来自 ' + escHtml(host) + "</span>" : "");
  var acts = document.createElement("div");
  acts.className = "brand-acts";
  var apply = document.createElement("button");
  apply.className = "btn brand-apply"; apply.type = "button";
  apply.textContent = "应用到当前项目";
  apply.setAttribute("aria-label", "应用品牌 " + name);
  apply.onclick = function () {
    apply.disabled = true;
    api("/api/agent", { message: "应用品牌 " + name }).then(function (r) {
      apply.disabled = false;
      log(r.reply || "已应用品牌", r.error ? "err" : "ok");
      return refresh(true);
    }).catch(function (e) {
      apply.disabled = false;
      log("应用失败：" + e.message, "err");
    });
  };
  var pin = document.createElement("button");
  pin.className = "btn brand-pin"; pin.type = "button";
  pin.textContent = b._default ? "取消默认" : "设为默认";
  pin.title = "新项目创建时自动应用该品牌";
  pin.onclick = function () {
    pin.disabled = true;
    api("/api/brand/default", { name: b._default ? "" : name })
      .then(function (r) {
        pin.disabled = false;
        log(r.default ? "默认品牌 → " + r.default : "已取消默认品牌", "ok");
        return brandRender();   // 重拉列表刷新 _default 标记
      }).catch(function (e) {
        pin.disabled = false;
        log("设置失败：" + e.message, "err");
      });
  };
  acts.appendChild(apply); acts.appendChild(pin);
  head.appendChild(back); head.appendChild(title); head.appendChild(acts);
  view.appendChild(head);

  /* Colors：v2 colors with roles（角色徽标 + 变量名）优先，
     v1 palette 纯色块兜底 */
  var kitColors = (b.colors || []).length ? b.colors :
      colors.map(function (c) { return { hex: c, role: "", var: "" }; });
  view.appendChild(sectionTitle("Colors " + kitColors.length));
  var sw = document.createElement("div");
  sw.className = "brand-colors";
  sw.innerHTML = kitColors.map(function (c) {
    return '<div class="brand-color"><i style="background:' +
        escHtml(c.hex) + '"></i>' +
        (c.role ? '<em class="brand-role">' + escHtml(c.role) + "</em>" : "") +
        "<code>" + escHtml(c.hex) +
        (c.var ? ' <u title="' + escHtml(c.var) + '">' +
            escHtml(c.var) + "</u>" : "") + "</code></div>";
  }).join("");
  view.appendChild(sw);

  /* Text styles：字号×字重组合（扫到的真实排版体系） */
  if ((b.text_styles || []).length) {
    view.appendChild(sectionTitle("Text styles " + b.text_styles.length));
    var tx = document.createElement("div");
    tx.className = "brand-fonts";
    tx.innerHTML = b.text_styles.map(function (t) {
      return '<div class="brand-font"><span style="font-size:' +
          Math.min(t.size, 22) + 'px;font-weight:' + t.weight + '">' +
          escHtml("标题 Style " + t.size) + "</span><code>" +
          t.size + "px · " + t.weight + "</code></div>";
    }).join("");
    view.appendChild(tx);
  }

  /* Backgrounds：大面积背景色（页面/表面） */
  if ((b.backgrounds || []).length) {
    view.appendChild(sectionTitle("Backgrounds " + b.backgrounds.length));
    var bgs = document.createElement("div");
    bgs.className = "brand-colors";
    bgs.innerHTML = b.backgrounds.map(function (g) {
      return '<div class="brand-color"><i style="background:' +
          escHtml(g.hex) + '"></i><code>' + escHtml(g.hex) +
          " ×" + g.count + "</code></div>";
    }).join("");
    view.appendChild(bgs);
  }

  /* Shapes and spacing：圆角/间距（扫到的真实形状体系） */
  if ((b.shapes || {}).radii && b.shapes.radii.length) {
    var radii = b.shapes.radii.concat(b.shapes.gaps || []);
    view.appendChild(sectionTitle("Shapes & spacing " + radii.length));
    var sh = document.createElement("div");
    sh.className = "brand-shapes";
    sh.innerHTML = radii.map(function (s) {
      var isGap = /px$/.test(s.value) && s.value !== undefined;
      return '<div class="brand-shape"><i style="border-radius:' +
          (isGap ? "2px" : escHtml(s.value)) + ';height:' +
          (isGap ? "10px" : "26px") + ';width:' +
          (isGap ? escHtml(s.value) : "40px") +
          ';background:var(--acc)"></i><code>' + escHtml(s.value) +
          " ×" + s.count + "</code></div>";
    }).join("");
    view.appendChild(sh);
  }

  /* Components：扫到的真实组件样式（.btn/.card 类规则） */
  if ((b.components || []).length) {
    view.appendChild(sectionTitle("Components " + b.components.length));
    var comps = document.createElement("div");
    comps.className = "brand-comps";
    comps.innerHTML = b.components.map(function (c) {
      return '<div class="bcomp"><span class="bcomp-tag">' +
          escHtml(c.kind) + '</span><div class="bcomp-stage">' +
          '<span style="background:' + escHtml(c.bg || "transparent") +
          ";color:" + escHtml(c.color || "inherit") +
          ";border-radius:" + escHtml(c.radius || "4px") +
          ';padding:4px 10px;font-size:11px">' +
          escHtml(c.selector.split(" ").pop()) + "</span></div></div>";
    }).join("");
    view.appendChild(comps);
  }

  /* Logos 集合（og:image / touch icon / favicon；本地下载的排前） */
  if (b.logo_image || (b.logos || []).length) {
    view.appendChild(sectionTitle("Logos " +
        ((b.logos || []).length || 1)));
    var lgx = document.createElement("div");
    lgx.className = "brand-logos";
    lgx.innerHTML = (b.logo_image
        ? '<img class="local" src="/api/brand-logo/' +
          encodeURIComponent(name) + '" alt="logo">' : "") +
        (b.logos || []).map(function (u) {
          return '<img src="' + escHtml(u) +
              '" alt="" onerror="this.style.display=\'none\'">';
        }).join("");
    view.appendChild(lgx);
  }

  /* Fonts：字体族示例（与 Text styles 的字号×字重组合互补） */
  if (fonts.length) {
    view.appendChild(sectionTitle("Fonts " + fonts.length));
    var fx = document.createElement("div");
    fx.className = "brand-fonts";
    fx.innerHTML = fonts.map(function (f) {
      return '<div class="brand-font"><span style="font-family:' +
          escHtml(String(f).replace(/"/g, "")) +
          ',sans-serif">Aa 品牌 Brand</span><code>' +
          escHtml(f) + "</code></div>";
    }).join("");
    view.appendChild(fx);
  }

  /* 组件预览卡（Title card / Lower third / Caption / End card）：
     用品牌色直接渲染的示例——真实模板在 compose 阶段消费同一批色。 */
  view.appendChild(sectionTitle("Components"));
  var prim = colors[0] || "#1d3557";
  var cards = document.createElement("div");
  cards.className = "brand-comps";
  cards.innerHTML =
    '<div class="bcomp"><span class="bcomp-tag">Title card</span>' +
    '<div class="bcomp-stage" style="background:' + escHtml(bg || "#101018") + '">' +
    '<div style="color:' + escHtml(ink || "#fff") + ';font-size:15px;' +
    'font-weight:700">标题在这里</div>' +
    '<i style="display:block;width:56%;height:3px;margin-top:6px;' +
    'background:' + escHtml(prim) + '"></i></div></div>' +
    '<div class="bcomp"><span class="bcomp-tag">Lower third</span>' +
    '<div class="bcomp-stage" style="background:' + escHtml(bg || "#101018") + '">' +
    '<span style="background:' + escHtml(prim) + ';color:' +
    escHtml(ink || "#fff") + ';padding:3px 8px;font-size:11px;' +
    'font-weight:700">名字 · 职务</span></div></div>' +
    '<div class="bcomp"><span class="bcomp-tag">Caption</span>' +
    '<div class="bcomp-stage" style="background:' + escHtml(bg || "#101018") + '">' +
    '<span style="background:rgba(0,0,0,.65);color:#fff;padding:3px 8px;' +
    'font-size:11px">字幕 · Made in <b style="color:' + escHtml(prim) +
    '">你的颜色</b></span></div></div>' +
    '<div class="bcomp"><span class="bcomp-tag">End card</span>' +
    '<div class="bcomp-stage" style="background:' + escHtml(bg || "#101018") + '">' +
    '<div style="color:' + escHtml(ink || "#fff") + ';font-size:13px;' +
    'font-weight:700">' + escHtml(b.logo_text || name) + "</div>" +
    '<span style="background:' + escHtml(prim) + ';color:' +
    escHtml(ink || "#fff") + ';padding:2px 8px;border-radius:9px;' +
    'font-size:10px;margin-top:5px;display:inline-block">' +
    escHtml(host || name) + "</span></div></div>";
  view.appendChild(cards);

  /* Context：其余字段一行表 + capture 证据行（HF 的
     "Read from bilibili.com · 10 colors · 5 text styles · 54 files"） */
  view.appendChild(sectionTitle("Context"));
  var ctx = document.createElement("div");
  ctx.className = "brand-ctx";
  var cap = b.capture || {};
  ctx.innerHTML =
    (cap.stylesheets || cap.css_vars
        ? '<div class="brand-capture">从 ' + escHtml(host || "本地文件") +
          " 抓取：样式表 " + escHtml(cap.stylesheets || 0) + " · CSS 变量 " +
          escHtml(cap.css_vars || 0) + " · 见过颜色 " +
          escHtml(cap.colors_seen || 0) + " · " +
          escHtml(cap.at || "") + "</div>" : "") +
    "<div><span>背景</span><code>" + escHtml(bg || "继承") + "</code></div>" +
    "<div><span>文字色</span><code>" + escHtml(ink || "继承") + "</code></div>" +
    "<div><span>音色</span><code>" + escHtml(b.voice || "默认") + "</code></div>" +
    (b.outro_text ? "<div><span>片尾文案</span><code>" +
        escHtml(b.outro_text) + "</code></div>" : "");
  view.appendChild(ctx);
}

function sectionTitle(txt) {
  var el = document.createElement("div");
  el.className = "brand-sec";
  el.textContent = txt;
  return el;
}
