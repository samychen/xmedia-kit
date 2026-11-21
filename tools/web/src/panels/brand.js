/* ---------------- 品牌 kits（HF Brand kits 左栏形态） ----------------
 * 列表 + 扫描表单（Scan my site）+ kit 详情（Colors/Text styles/
 * Logo/组件预览卡）+ 应用到当前项目 / 设为默认（default pin）。
 * 扫描走 POST /api/brand/scan；应用复用 /api/agent「应用品牌 <名>」
 * 规则引擎指令（与模板插入同一编辑链）；默认走 /api/brand/default。 */
import { api } from "../shared/api-client.js";
import { $, escHtml, log } from "../shared/dom.js";
import { mk } from "../timeline/model.js";
import { fitPreview } from "../preview/zoom.js";
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
    var open = function () { brandMainOpen(b); };
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

  /* HF 9 类编号导航（左栏窄宽适配：横排 chip + 锚点滚动） */
  var filesN = (b._files || []).length;
  var rulesN = (b.rules_txt || []).length + (b.sources || []).length;
  var navDefs = [
    ["sec-logos", "01", "Logos", (b.logos || []).length || (b.logo_image ? 1 : 0)],
    ["sec-colors", "02", "Colors", kitColorsLen(b)],
    ["sec-styles", "03", "Text styles", (b.text_styles || []).length],
    ["sec-bgs", "04", "Backgrounds", (b.backgrounds || []).length],
    ["sec-shapes", "05", "Shapes & spacing",
     ((b.shapes || {}).radii || []).length + ((b.shapes || {}).gaps || []).length],
    ["sec-comps", "06", "Components", (b.components || []).length],
    ["sec-shots", "07", "Screenshots", b.screenshot ? 1 : 0],
    ["sec-files", "08", "Files", filesN],
    ["sec-rules", "09", "Rules & sources", rulesN]
  ];
  var nav = document.createElement("div");
  nav.className = "brand-nav";
  nav.innerHTML = navDefs.map(function (nd) {
    return '<button class="brand-navi" data-target="' + nd[0] +
        '" type="button"><em>' + nd[1] + '</em>' + escHtml(nd[2]) +
        '<u>' + nd[3] + '</u></button>';
  }).join("");
  view.appendChild(nav);
  nav.onclick = function (ev) {
    var btn = ev.target.closest(".brand-navi");
    if (!btn) return;
    var sec = document.getElementById(btn.dataset.target);
    if (sec) sec.scrollIntoView({ block: "start", behavior: "smooth" });
  };

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
    view.appendChild(sectionTitle("Text styles " + b.text_styles.length, "sec-styles"));
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
    view.appendChild(sectionTitle("Backgrounds " + b.backgrounds.length, "sec-bgs"));
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
    view.appendChild(sectionTitle("Shapes & spacing " + radii.length, "sec-shapes"));
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
    view.appendChild(sectionTitle("Components " + b.components.length, "sec-comps"));
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

  /* Screenshots：网页首屏视觉证据（scan 时 html_bake 烘的截图） */
  if (b.screenshot) {
    view.appendChild(sectionTitle("Screenshots 1"));
    var sh2 = document.createElement("div");
    sh2.className = "brand-shots";
    sh2.innerHTML = '<img src="/api/brand-shot/' + encodeURIComponent(name) +
        '" alt="home screenshot">';
    view.appendChild(sh2);
  }

  /* Logos 集合（og:image / touch icon / favicon；本地下载的排前） */
  if (b.logo_image || (b.logos || []).length) {
    view.appendChild(sectionTitle("Logos " +
        ((b.logos || []).length || 1), "sec-logos"));
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

  /* Files：kit 专属落盘文件清单（HF Files 分类） */
  if ((b._files || []).length) {
    view.appendChild(sectionTitle("Files " + b._files.length, "sec-files"));
    var fl = document.createElement("div");
    fl.className = "brand-ctx";
    fl.innerHTML = b._files.map(function (f) {
      return "<div><span>" + escHtml(f.path) + "</span><code>" +
          (f.bytes / 1024).toFixed(1) + " KB</code></div>";
    }).join("");
    view.appendChild(fl);
  }

  /* Rules and sources：自动规则 + 抓取来源（HF Rules 分类） */
  if ((b.rules_txt || []).length || (b.sources || []).length) {
    view.appendChild(sectionTitle("Rules & sources", "sec-rules"));
    var rl = document.createElement("div");
    rl.className = "brand-rules";
    rl.innerHTML = (b.rules_txt || []).map(function (r) {
      return '<div class="brand-rule"><b>§</b>' + escHtml(r) + "</div>";
    }).join("") + (b.sources || []).map(function (s) {
      return '<div class="brand-rule src"><b>⌘</b>' + escHtml(s.what) +
          (s.url ? ' <code>' + escHtml(String(s.url).slice(0, 40)) +
           '</code>' : "") + "</div>";
    }).join("");
    view.appendChild(rl);
  }

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


/* ---------------- 品牌详情主区大页（HF kit 页同形态） ----------------
 * center 主区整体切换为品牌详情：左列 01-09 编号导航 + 右侧当前
 * 分类内容（切换式）。关闭 = 回编辑器预览。 */
var mainKit = null, mainSection = "sec-colors";

export function brandMainOpen(b) {
  mainKit = b;
  mainSection = "sec-colors";
  var view = $("brand-main");
  $("pvwrap").style.display = "none";
  /* kit 页占满主区+右区（HF kit 页形态）——Inspector 暂隐 */
  document.querySelector(".right").style.display = "none";
  document.querySelector(".workspace").classList.add("kit-page");
  view.style.display = "flex";
  brandMainRender();
}

export function brandMainClose() {
  var view = $("brand-main");
  if (!view || view.style.display === "none") return;
  view.style.display = "none";
  $("pvwrap").style.display = "flex";
  document.querySelector(".right").style.display = "";
  document.querySelector(".workspace").classList.remove("kit-page");
  mainKit = null;
  fitPreview();
}

function brandMainRender() {
  var view = $("brand-main");
  var b = mainKit;
  if (!view || !b) return;
  var name = b.name || "";
  view.replaceChildren();

  var head = mk("div", "bm-head");
  var close = mk("button", "bm-close");
  close.type = "button"; close.textContent = "← 返回编辑器";
  close.onclick = function () { brandMainClose(); };
  var logo = b.logo_image ?
    '<img class="bm-logo" src="/api/brand-logo/' + encodeURIComponent(name) +
    '" alt="" onerror="this.style.display=\'none\'">' : "";
  head.innerHTML = logo +
    '<div class="bm-id"><div class="bm-name">' + escHtml(name) + "</div>" +
    '<div class="bm-src">From ' +
    escHtml(String(b.source_url || "").replace(/^https?:\/\//, "")) +
    " · brandkit-v2 capture</div></div>" +
    '<div class="sp"></div>' +
    '<button class="bm-del" type="button" title="删除此品牌 kit">删除 kit</button>';
  head.querySelector(".bm-del").onclick = function () {
    if (!confirm("删除品牌 kit「" + name + "」？logo/截图等文件一并删除。")) return;
    api("/api/brand/delete", { name: name }).then(function () {
      log("品牌 kit 已删除：" + name, "ok");
      brandMainClose();
    }).catch(function (e) {
      log("删除失败：" + e.message, "err");
    });
  };
  head.appendChild(close);
  view.appendChild(head);

  var wrap = mk("div", "bm-wrap");
  var navList = navDefsFor(b);
  var navCol = mk("div", "bm-nav");
  navCol.innerHTML = navList.map(function (nd) {
    return '<button class="bm-navi" data-target="' + nd[0] + '" type="button">' +
      '<em>' + nd[1] + "</em><span>" + escHtml(nd[2]) + "</span><u>" +
      nd[3] + "</u></button>";
  }).join("");
  navCol.onclick = function (ev) {
    var btn = ev.target.closest(".bm-navi");
    if (!btn) return;
    mainSection = btn.dataset.target;
    brandMainRender();
  };
  wrap.appendChild(navCol);
  var content = mk("div", "bm-content");
  content.id = "bm-content";
  wrap.appendChild(content);
  view.appendChild(wrap);

  renderKitSection(content, b, mainSection);
}

function navDefsFor(b) {
  return [
    ["sec-logos", "01", "Logos", (b.logos || []).length || (b.logo_image ? 1 : 0)],
    ["sec-colors", "02", "Colors", kitColorsLen(b)],
    ["sec-styles", "03", "Text styles", (b.text_styles || []).length],
    ["sec-bgs", "04", "Backgrounds", (b.backgrounds || []).length],
    ["sec-shapes", "05", "Shapes & spacing",
     ((b.shapes || {}).radii || []).length + ((b.shapes || {}).gaps || []).length],
    ["sec-comps", "06", "Components", (b.components || []).length],
    ["sec-shots", "07", "Screenshots", b.screenshot ? 1 : 0],
    ["sec-files", "08", "Files", (b._files || []).length],
    ["sec-rules", "09", "Rules & sources",
     (b.rules_txt || []).length + (b.sources || []).length]
  ];
}

/* 单分类宽版渲染（主区右列） */
function renderKitSection(box, b, sec) {
  var name = b.name || "";
  var colors = (b.colors || []).length ? b.colors :
      (b.palette || []).map(function (c) { return { hex: c, role: "", var: "" }; });
  var ink = b.ink || b.text_color || "";
  var bg = b.background || "";
  var fonts = b.fonts || [];
  var host = String(b.source_url || "").replace(/^https?:\/\//, "");
  box.replaceChildren();
  var prim = colors.length ? colors[0].hex : "#1d3557";

  if (sec === "sec-logos") {
    if (b.logo_image) {
      box.innerHTML = '<div class="bm-big-logo"><img src="/api/brand-logo/' +
        encodeURIComponent(name) + '" alt="logo"></div>';
    }
    var holder = box.querySelector(".bm-big-logo") || box;
    (b.logos || []).forEach(function (u) {
      var im = document.createElement("img");
      im.src = u; im.className = "bm-logo-item";
      im.onerror = function () { im.remove(); };
      holder.appendChild(im);
    });
    if (!holder.children.length) {
      box.innerHTML = '<div class="sb-empty">无 logo</div>';
    }
    return;
  }
  if (sec === "sec-colors") {
    box.innerHTML = '<div class="bm-colors">' + colors.map(function (c) {
      return '<div class="bm-color"><i style="background:' + escHtml(c.hex) +
        '"></i>' + (c.role ? '<em class="brand-role">' + escHtml(c.role) +
        "</em>" : "") + "<code>" + escHtml(c.hex) +
        (c.var ? " <u>" + escHtml(c.var) + "</u>" : "") + "</code></div>";
    }).join("") + "</div>";
    return;
  }
  if (sec === "sec-styles") {
    box.innerHTML = '<div class="brand-fonts">' + (b.text_styles || []).map(
      function (f) {
        return '<div class="brand-font"><span style="font-size:' +
          Math.min(f.size * 1.4, 46) + 'px;font-weight:' + f.weight + '">' +
          escHtml("标题 Style " + f.size) + "</span><code>" + f.size +
          "px · " + f.weight + "</code></div>";
      }).join("") + "</div>";
    return;
  }
  if (sec === "sec-bgs") {
    box.innerHTML = '<div class="bm-colors">' + (b.backgrounds || []).map(
      function (g) {
        return '<div class="bm-color"><i style="background:' + escHtml(g.hex) +
          '"></i><code>' + escHtml(g.hex) + " ×" + g.count + "</code></div>";
      }).join("") + "</div>";
    return;
  }
  if (sec === "sec-shapes") {
    var all = ((b.shapes || {}).radii || []).concat((b.shapes || {}).gaps || []);
    box.innerHTML = '<div class="brand-shapes">' + all.map(function (s) {
      return '<div class="brand-shape"><i style="border-radius:' +
        escHtml(s.value) + ';width:64px;height:34px;background:var(--acc)">'
        + "</i><code>" + escHtml(s.value) + " ×" + s.count + "</code></div>";
    }).join("") + "</div>";
    return;
  }
  if (sec === "sec-comps") {
    box.innerHTML = (b.components || []).length ?
      '<div class="brand-comps">' + b.components.map(function (c) {
        return '<div class="bcomp"><span class="bcomp-tag">' +
          escHtml(c.kind) + '</span><div class="bcomp-stage">' +
          '<span style="background:' + escHtml(c.bg || "transparent") +
          ";color:" + escHtml(c.color || "inherit") + ";border-radius:" +
          escHtml(c.radius || "4px") + ';padding:4px 10px;font-size:11px">' +
          escHtml(c.selector.split(" ").pop()) + "</span></div></div>";
      }).join("") + "</div>" :
      '<div class="sb-empty">该站类名未匹配到组件启发式——启发式仅认 '
      + "btn/card/tag 类名</div>";
    return;
  }
  if (sec === "sec-shots") {
    box.innerHTML = b.screenshot ?
      '<div class="bm-shot"><img src="/api/brand-shot/' +
      encodeURIComponent(name) + '" alt="home"></div>' :
      '<div class="sb-empty">该站对无头渲染反爬——截图不可得（如实）</div>';
    return;
  }
  if (sec === "sec-files") {
    box.innerHTML = '<div class="brand-ctx">' + (b._files || []).map(
      function (f) {
        return "<div><span>" + escHtml(f.path) + "</span><code>" +
          (f.bytes / 1024).toFixed(1) + " KB</code></div>";
      }).join("") + "</div>";
    return;
  }
  if (sec === "sec-rules") {
    box.innerHTML = '<div class="brand-rules">' +
      (b.rules_txt || []).map(function (r) {
        return '<div class="brand-rule"><b>§</b>' + escHtml(r) + "</div>";
      }).join("") + (b.sources || []).map(function (s) {
        return '<div class="brand-rule src"><b>⌘</b>' + escHtml(s.what) +
          "</div>";
      }).join("") + "</div>";
    return;
  }
  box.innerHTML = '<div class="sb-empty">—</div>';
}

function kitColorsLen(b) {
  return (b.colors || []).length || (b.palette || []).length;
}

function sectionTitle(txt, id) {
  var el = document.createElement("div");
  el.className = "brand-sec";
  el.textContent = txt;
  if (id) el.id = id;
  return el;
}
