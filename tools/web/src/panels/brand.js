/* ---------------- 品牌 kits（HF Brand kits 左栏形态） ----------------
 * 列表 + 扫描表单（Scan my site）+ kit 详情（Colors/Text styles/
 * Logo/组件预览卡）+ 应用到当前项目 / 设为默认（default pin）。
 * 扫描走 POST /api/brand/scan；应用复用 /api/agent「应用品牌 <名>」
 * 规则引擎指令（与模板插入同一编辑链）；默认走 /api/brand/default。 */
import { api } from "../shared/api-client.js";
import { $, escHtml, log } from "../shared/dom.js";
import { mk } from "../timeline/model.js";
import { fitPreview } from "../preview/zoom.js";
import { refresh, refreshBrandChip } from "./status.js";

function deleteBrandKit(b) {
  var name = (b && b.name) || (b && b._id) || "";
  var id = (b && b._id) || name;
  if (!id) return;
  if (!confirm("删除品牌 kit「" + name + "」？logo/截图等文件一并删除。")) return;
  api("/api/brand/delete", { name: id }).then(function () {
    log("品牌 kit 已删除：" + name, "ok");
    brandMainClose();
    refreshBrandChip();
    brandRender();
  }).catch(function (e) {
    log("删除失败：" + e.message, "err");
  });
}

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
      refreshBrandChip();
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
      '<span class="sp"></span>' +
      '<button class="brand-card-del" type="button" title="删除此品牌 kit" aria-label="删除 ' +
      escHtml(b.name || "") + '">×</button>' +
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
    var delBtn = card.querySelector(".brand-card-del");
    if (delBtn) delBtn.onclick = function (ev) {
      ev.stopPropagation();
      deleteBrandKit(b);
    };
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
  var mkVid = document.createElement("button");
  mkVid.className = "btn acc brand-make-video"; mkVid.type = "button";
  mkVid.textContent = "✨ 用此站点做视频";
  mkVid.title = "基于扫描到的网页文案、截图与品牌色板生成多镜头发布视频";
  mkVid.onclick = function () {
    mkVid.disabled = true;
    mkVid.textContent = "生成中…";
    api("/api/brand/create-video", { name: name }).then(function (r) {
      log("已生成网站视频项目：" + r.name + "（" + r.scenes + " 镜）", "ok");
      location.href = r.url + "?fresh=1";
    }).catch(function (e) {
      mkVid.disabled = false;
      mkVid.textContent = "✨ 用此站点做视频";
      log("生成视频失败：" + e.message, "err");
    });
  };
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
  acts.appendChild(mkVid); acts.appendChild(apply); acts.appendChild(pin);
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
  if (!view) return;
  var pv = $("pvwrap");
  if (pv) pv.style.display = "none";
  /* kit 页占满主区+右区（HF kit 页形态）——Inspector 暂隐 */
  var right = document.querySelector(".right");
  if (right) right.style.display = "none";
  var ws = document.querySelector(".workspace");
  if (ws) ws.classList.add("kit-page");
  view.style.display = "flex";
  brandMainRender();
}

export function brandMainClose() {
  var view = $("brand-main");
  if (!view || view.style.display === "none") return;
  view.style.display = "none";
  var pv = $("pvwrap");
  if (pv) pv.style.display = "flex";
  var right = document.querySelector(".right");
  if (right) right.style.display = "";
  var ws = document.querySelector(".workspace");
  if (ws) ws.classList.remove("kit-page");
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
    " · " + escHtml((b.capture || {}).engine || "brandkit-v2") +
    "</div></div>" +
    '<div class="sp"></div>' +
    '<button class="btn acc bm-make-vid" type="button" title="根据该品牌与网站文案直接生成发布视频">✨ 用此站点生成视频</button>' +
    '<button class="bm-del" type="button" title="删除此品牌 kit">删除 kit</button>';
  var makeVidBtn = head.querySelector(".bm-make-vid");
  if (makeVidBtn) makeVidBtn.onclick = function () {
    makeVidBtn.disabled = true;
    makeVidBtn.textContent = "生成中…";
    api("/api/brand/create-video", { name: name }).then(function (r) {
      log("已生成网站视频项目：" + r.name + "（" + r.scenes + " 镜）", "ok");
      location.href = r.url + "?fresh=1";
    }).catch(function (e) {
      makeVidBtn.disabled = false;
      makeVidBtn.textContent = "✨ 用此站点生成视频";
      log("生成视频失败：" + e.message, "err");
    });
  };
  var del = head.querySelector(".bm-del");
  if (del) del.onclick = function () {
    deleteBrandKit(b);
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

function saveBrandEdit(b, patch, okMsg) {
  var id = (b && (b._id || b.name)) || "";
  if (!id) return Promise.resolve();
  return api("/api/brand/update", Object.assign({ name: id }, patch))
    .then(function () {
      log(okMsg || "品牌 kit 已更新", "ok");
      refreshBrandChip();
      return api("/api/brands").then(function (listRes) {
        var kits = listRes.brands || [];
        var hit = kits.find(function (k) { return (k._id || k.name) === id; });
        if (hit) mainKit = hit;
        brandMainRender();
        return brandRender();
      });
    })
    .catch(function (e) {
      log("保存失败：" + e.message, "err");
    });
}

function navDefsFor(b) {
  var surfs = (b.product_surfaces || []).length || (b.screenshot ? 1 : 0);
  return [
    ["sec-logos", "01", "Logos", (b.logos || []).length || (b.logo_image ? 1 : 0)],
    ["sec-colors", "02", "Colors", kitColorsLen(b)],
    ["sec-styles", "03", "Text styles", (b.text_styles || []).length],
    ["sec-bgs", "04", "Backgrounds", (b.backgrounds || []).length],
    ["sec-shapes", "05", "Shapes & spacing",
     ((b.shapes || {}).radii || []).length + ((b.shapes || {}).gaps || []).length],
    ["sec-comps", "06", "Components", (b.components || []).length],
    ["sec-shots", "07", "Surfaces & shots", surfs],
    ["sec-files", "08", "Files & context", (b._files || []).length],
    ["sec-rules", "09", "Voice & rules",
     (b.rules_txt || []).length + (b.sources || []).length]
  ];
}

/* 单分类宽版渲染（主区右列：支持交互式编辑） */
function renderKitSection(box, b, sec) {
  var name = b.name || "";
  var id = b._id || name;
  var colors = (b.colors || []).length ? b.colors.slice() :
      (b.palette || []).map(function (c) { return { hex: c, role: "", var: "" }; });
  box.replaceChildren();

  if (sec === "sec-logos") {
    var bar = mk("div", "bm-toolbar");
    var upLogo = document.createElement("input");
    upLogo.type = "file"; upLogo.accept = "image/*"; upLogo.style.display = "none";
    var upBtn = mk("button", "btn"); upBtn.type = "button";
    upBtn.textContent = "+ 上传替换 Logo";
    upBtn.onclick = function () { upLogo.click(); };
    upLogo.onchange = function () {
      var f = upLogo.files && upLogo.files[0];
      if (!f) return;
      var rd = new FileReader();
      rd.onload = function () {
        saveBrandEdit(b, { upload_logo: { filename: f.name, data_url: String(rd.result || "") } },
                      "已更新品牌 Logo");
      };
      rd.readAsDataURL(f);
    };
    bar.appendChild(upLogo); bar.appendChild(upBtn);
    box.appendChild(bar);
    if ((b.logo_marks || []).length) {
      var ml = mk("div", "bm-mark-list");
      (b.logo_marks || []).forEach(function (m) {
        var pill = mk("span", "bm-mark-pill");
        pill.textContent = (m.kind || "mark") + " · " + (m.label || "Logo");
        ml.appendChild(pill);
      });
      box.appendChild(ml);
    }
    var holder = mk("div", "bm-big-logo");
    if (b.logo_image) {
      var mainImg = document.createElement("img");
      mainImg.src = "/api/brand-logo/" + encodeURIComponent(id) + "?t=" + Date.now();
      mainImg.alt = "logo";
      holder.appendChild(mainImg);
    }
    (b.logos || []).forEach(function (u) {
      var im = document.createElement("img");
      im.src = u; im.className = "bm-logo-item";
      im.onerror = function () { im.remove(); };
      holder.appendChild(im);
    });
    if (holder.children.length) box.appendChild(holder);
    else {
      var em = mk("div", "sb-empty"); em.textContent = "无 logo——点上方上传";
      box.appendChild(em);
    }
    return;
  }
  if (sec === "sec-colors") {
    var addBar = mk("div", "bm-toolbar");
    var cPick = document.createElement("input");
    cPick.type = "color"; cPick.value = "#00aeec";
    var rSel = document.createElement("select");
    rSel.innerHTML = '<option value="primary">primary（主色）</option>' +
      '<option value="accent">accent（强调色）</option>' +
      '<option value="secondary">secondary（辅色）</option>' +
      '<option value="background">background（背景）</option>' +
      '<option value="ink">ink（文字）</option><option value="">无角色</option>';
    var addCol = mk("button", "btn acc"); addCol.type = "button";
    addCol.textContent = "+ 添加颜色";
    addCol.onclick = function () {
      var next = colors.concat([{ hex: cPick.value, role: rSel.value, var: "custom" }]);
      saveBrandEdit(b, { colors: next }, "已添加品牌色 " + cPick.value);
    };
    addBar.appendChild(cPick); addBar.appendChild(rSel); addBar.appendChild(addCol);
    box.appendChild(addBar);

    var grid = mk("div", "bm-colors");
    var roles = ["", "primary", "secondary", "accent", "background", "ink", "muted"];
    colors.forEach(function (c, idx) {
      var card = mk("div", "bm-color");
      var sw = document.createElement("i");
      sw.style.background = c.hex;
      card.appendChild(sw);
      var code = document.createElement("code");
      code.textContent = c.hex + (c.var ? " · " + c.var : "");
      card.appendChild(code);
      var foot = mk("div", "bm-color-foot");
      var sel = mk("select", "bm-role-sel");
      roles.forEach(function (rl) {
        var op = document.createElement("option");
        op.value = rl; op.textContent = rl || "未分配角色";
        sel.appendChild(op);
      });
      sel.value = roles.includes(c.role || "") ? (c.role || "") : "";
      sel.onchange = function () {
        var next = colors.slice();
        next[idx] = Object.assign({}, next[idx], { role: sel.value });
        saveBrandEdit(b, { colors: next }, "已更新色值角色");
      };
      var del = mk("button", "bm-col-del"); del.type = "button";
      del.textContent = "×"; del.title = "移除色值";
      del.onclick = function () {
        var next = colors.filter(function (_x, j) { return j !== idx; });
        saveBrandEdit(b, { colors: next }, "已移除颜色 " + c.hex);
      };
      foot.appendChild(sel); foot.appendChild(del);
      card.appendChild(foot);
      grid.appendChild(card);
    });
    box.appendChild(grid);
    return;
  }
  if (sec === "sec-styles") {
    var fontSt = document.getElementById("bm-font-faces");
    if (!fontSt) {
      fontSt = document.createElement("style");
      fontSt.id = "bm-font-faces";
      document.head.appendChild(fontSt);
    }
    fontSt.textContent = (b.font_files || []).map(function (ff) {
      return "@font-face { font-family: '" + String(ff.family || "").replace(/'/g, "") +
        "'; font-weight: " + (ff.weight || 400) + "; src: url('/api/brand-file/" +
        encodeURIComponent(id) + "?raw=1&path=" + encodeURIComponent(ff.path || "") + "'); }";
    }).join("\n");

    var tBar = mk("div", "bm-toolbar");
    var rRole = document.createElement("select");
    rRole.innerHTML = '<option value="display">display（主标题）</option>' +
      '<option value="h1">h1（大标题）</option><option value="heading">heading（小标题）</option>' +
      '<option value="body">body（正文）</option><option value="label">label（标签）</option>';
    var rFam = document.createElement("input");
    rFam.type = "text"; rFam.placeholder = "字体族，如 PingFang SC";
    rFam.value = (b.fonts && b.fonts[0]) || "";
    var rSz = document.createElement("input");
    rSz.type = "number"; rSz.value = "32"; rSz.style.width = "64px"; rSz.title = "字号(px)";
    var rWt = document.createElement("input");
    rWt.type = "number"; rWt.value = "700"; rWt.step = "100"; rWt.style.width = "68px"; rWt.title = "字重";
    var addTsBtn = mk("button", "btn acc"); addTsBtn.type = "button";
    addTsBtn.textContent = "+ 添加字样";
    addTsBtn.onclick = function () {
      var curTs = (b.text_styles || []).slice();
      curTs.unshift({
        role: rRole.value, font_family: rFam.value.trim(),
        size: parseInt(rSz.value, 10) || 24, weight: parseInt(rWt.value, 10) || 600,
        line_height: 1.35, sample: "Brand Typography 品牌排版示例",
      });
      saveBrandEdit(b, { text_styles: curTs }, "已添加排版样式");
    };
    var upFont = document.createElement("input");
    upFont.type = "file"; upFont.accept = ".woff2,.woff,.ttf,.otf"; upFont.style.display = "none";
    var upFontBtn = mk("button", "btn"); upFontBtn.type = "button";
    upFontBtn.textContent = "+ 上传字体文件 (.woff2/.ttf)";
    upFontBtn.onclick = function () { upFont.click(); };
    upFont.onchange = function () {
      var f = upFont.files && upFont.files[0];
      if (!f) return;
      var fam = rFam.value.trim() || f.name.replace(/\.[^.]+$/, "");
      var rd = new FileReader();
      rd.onload = function () {
        saveBrandEdit(b, { upload_font: {
          filename: f.name, family: fam, weight: parseInt(rWt.value, 10) || 400,
          data_url: String(rd.result || ""),
        } }, "已托管字体文件 " + f.name);
      };
      rd.readAsDataURL(f);
    };
    tBar.appendChild(rRole); tBar.appendChild(rFam); tBar.appendChild(rSz);
    tBar.appendChild(rWt); tBar.appendChild(addTsBtn);
    tBar.appendChild(upFont); tBar.appendChild(upFontBtn);
    box.appendChild(tBar);

    var tsList = mk("div", "brand-fonts");
    var tStyles = b.text_styles || [];
    if (!tStyles.length) {
      var emTs = mk("div", "sb-empty");
      emTs.textContent = "暂无排版角色——可点上方添加";
      tsList.appendChild(emTs);
    }
    tStyles.forEach(function (f, idx) {
      var row = mk("div", "brand-font bm-ts-row");
      var fam = f.font_family || (b.fonts && b.fonts[0]) || "inherit";
      var roleTag = f.role || (f.size >= 24 ? "heading" : "body");
      var lhs = f.line_height ? " · 行高 " + f.line_height : "";
      var smp = f.sample || (roleTag === "display" ? (b.logo_text || name) : "让每一帧构图都有质感 · Typography");
      var preview = document.createElement("div");
      preview.className = "bm-ts-main";
      preview.innerHTML = '<div class="bm-ts-meta"><em class="bm-role-pill">' + escHtml(roleTag) +
        "</em><code>" + escHtml(fam) + " · " + f.size + "px · " + f.weight + escHtml(lhs) +
        "</code></div>" +
        '<div class="bm-ts-sample" style="font-family:\'' + escHtml(fam) + '\', sans-serif;font-size:' +
        Math.min(Math.max(f.size * 1.1, 13), 42) + "px;font-weight:" + (f.weight || 400) +
        ";line-height:" + (f.line_height || 1.35) + '">' + escHtml(smp) + "</div>";
      var rm = mk("button", "bm-col-del"); rm.type = "button"; rm.textContent = "×";
      rm.onclick = function () {
        var next = tStyles.filter(function (_x, j) { return j !== idx; });
        saveBrandEdit(b, { text_styles: next }, "已移除字样");
      };
      row.appendChild(preview); row.appendChild(rm);
      tsList.appendChild(row);
    });
    box.appendChild(tsList);
    if ((b.font_files || []).length) {
      var ffBox = mk("div", "brand-ctx");
      ffBox.style.marginTop = "12px";
      (b.font_files || []).forEach(function (ff) {
        var fr = document.createElement("div");
        fr.innerHTML = "<span>🔤 " + escHtml(ff.family) + " (" + (ff.weight || 400) + ") · " +
          escHtml(ff.path) + "</span><code>" + ((ff.bytes || 0) / 1024).toFixed(1) + " KB</code>";
        var rm = mk("button", "bm-col-del"); rm.type = "button"; rm.textContent = "×";
        rm.onclick = function () { saveBrandEdit(b, { remove_font: ff.path }, "已移除字体文件"); };
        fr.appendChild(rm);
        ffBox.appendChild(fr);
      });
      box.appendChild(ffBox);
    }
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
    var cBar = mk("div", "bm-toolbar");
    var cKind = document.createElement("select");
    cKind.innerHTML = '<option value="button">button（按钮）</option>' +
      '<option value="card">card（卡片）</option><option value="tag">tag（标签）</option>' +
      '<option value="input">input（输入框）</option>';
    var cLbl = document.createElement("input");
    cLbl.type = "text"; cLbl.placeholder = "示例文案，如 立即投稿"; cLbl.style.width = "130px";
    var cBg = document.createElement("input");
    cBg.type = "color"; cBg.value = (b.palette && b.palette[0]) || "#00aeec"; cBg.title = "底色";
    var cTxt = document.createElement("input");
    cTxt.type = "color"; cTxt.value = "#ffffff"; cTxt.title = "文字色";
    var cRad = document.createElement("input");
    cRad.type = "text"; cRad.value = "8px"; cRad.style.width = "58px"; cRad.title = "圆角";
    var addComp = mk("button", "btn acc"); addComp.type = "button";
    addComp.textContent = "+ 添加组件";
    addComp.onclick = function () {
      var cur = (b.components || []).slice();
      cur.unshift({
        kind: cKind.value, selector: ".custom-" + cKind.value,
        label: cLbl.value.trim() || "示例组件",
        bg: cBg.value, color: cTxt.value, radius: cRad.value.trim() || "6px",
        border: "", shadow: "", font_size: 13, font_weight: 600,
      });
      saveBrandEdit(b, { components: cur }, "已添加品牌组件");
    };
    cBar.appendChild(cKind); cBar.appendChild(cLbl); cBar.appendChild(cBg);
    cBar.appendChild(cTxt); cBar.appendChild(cRad); cBar.appendChild(addComp);
    box.appendChild(cBar);

    var comps = b.components || [];
    if (!comps.length) {
      var emC = mk("div", "sb-empty");
      emC.textContent = "未检测到匹配组件，可点上方手动添加或重扫主页";
      box.appendChild(emC);
      return;
    }
    var stageBg = b.background || "#18191c";
    var cGrid = mk("div", "brand-comps bm-comps-grid");
    comps.forEach(function (c, idx) {
      var card = mk("div", "bcomp");
      var top = mk("div", "bm-comp-head");
      top.innerHTML = '<span class="bcomp-tag">' + escHtml(c.kind) +
        "</span><code>" + escHtml((c.selector || "").split(" ").pop()) + "</code>";
      var rm = mk("button", "bm-col-del"); rm.type = "button"; rm.textContent = "×";
      rm.onclick = function () {
        saveBrandEdit(b, { components: comps.filter(function (_x, j) { return j !== idx; }) },
                      "已移除组件");
      };
      top.appendChild(rm);
      var stage = mk("div", "bcomp-stage");
      stage.style.background = stageBg;
      var demo = document.createElement(c.kind === "input" ? "div" : "span");
      demo.className = "bm-comp-preview";
      demo.textContent = c.label || (c.kind === "card" ? "内容卡片示例" : "按钮示例");
      if (c.bg_image) demo.style.backgroundImage = c.bg_image;
      else if (c.bg) demo.style.background = c.bg;
      if (c.color) demo.style.color = c.color;
      if (c.border) demo.style.border = c.border;
      if (c.radius) demo.style.borderRadius = c.radius;
      if (c.shadow) demo.style.boxShadow = c.shadow;
      if (c.font_weight) demo.style.fontWeight = String(c.font_weight);
      stage.appendChild(demo);
      var meta = document.createElement("div");
      meta.className = "bm-comp-meta";
      meta.textContent = [c.bg || (c.border ? "描边" : ""), c.radius || "0px"].filter(Boolean).join(" · ");
      card.appendChild(top); card.appendChild(stage); card.appendChild(meta);
      cGrid.appendChild(card);
    });
    box.appendChild(cGrid);
    return;
  }
  if (sec === "sec-shots") {
    var sum = b.site_summary || {};
    var sumHtml = (sum.hero_title || sum.description) ? (
      '<div class="brand-ctx" style="margin-bottom:12px">' +
      (sum.hero_title ? "<div><span>站点大标题</span><b>" + escHtml(sum.hero_title) + "</b></div>" : "") +
      (sum.hero_subtitle ? "<div><span>副标题/简介</span><code>" + escHtml(sum.hero_subtitle) + "</code></div>" : "") +
      ((sum.bullets || []).length ? "<div><span>要点提取</span><code>" + escHtml(sum.bullets.join(" / ")) + "</code></div>" : "") +
      "</div>"
    ) : "";
    if (sumHtml) {
      var sumWrap = document.createElement("div");
      sumWrap.innerHTML = sumHtml;
      box.appendChild(sumWrap);
    }
    var sBar = mk("div", "bm-toolbar");
    var lblInp = document.createElement("input");
    lblInp.type = "text"; lblInp.placeholder = "截图标签，如 功能界面";
    var upShot = document.createElement("input");
    upShot.type = "file"; upShot.accept = "image/png,image/jpeg,image/webp";
    upShot.style.display = "none";
    var upShotBtn = mk("button", "btn"); upShotBtn.type = "button";
    upShotBtn.textContent = "+ 上传产品界面图";
    upShotBtn.onclick = function () { upShot.click(); };
    upShot.onchange = function () {
      var f = upShot.files && upShot.files[0];
      if (!f) return;
      var rd = new FileReader();
      rd.onload = function () {
        saveBrandEdit(b, { add_surface: {
          filename: f.name, label: lblInp.value.trim() || f.name,
          data_url: String(rd.result || ""),
        } }, "已补充产品界面图");
      };
      rd.readAsDataURL(f);
    };
    sBar.appendChild(lblInp); sBar.appendChild(upShot); sBar.appendChild(upShotBtn);
    box.appendChild(sBar);

    var surfs = (b.product_surfaces && b.product_surfaces.length)
      ? b.product_surfaces
      : (b.screenshot ? [{ path: b.screenshot, label: "首页首屏概览" }] : []);
    if (surfs.length) {
      var sGrid = mk("div", "bm-shots-grid");
      surfs.forEach(function (sf) {
        var card = mk("div", "bm-shot-card");
        var im = document.createElement("img");
        im.src = "/api/brand-shot/" + encodeURIComponent(id) +
          "?file=" + encodeURIComponent(sf.path || "");
        im.alt = sf.label || "surface";
        card.appendChild(im);
        var meta = mk("div", "bm-shot-meta");
        var span = document.createElement("span");
        span.textContent = sf.label || sf.path;
        var rm = mk("button", "bm-col-del"); rm.type = "button";
        rm.textContent = "×"; rm.title = "删除截图";
        rm.onclick = function () {
          saveBrandEdit(b, { remove_surface: sf.path }, "已移除界面截图");
        };
        meta.appendChild(span); meta.appendChild(rm);
        card.appendChild(meta);
        sGrid.appendChild(card);
      });
      box.appendChild(sGrid);
    } else {
      var noShot = mk("div", "sb-empty");
      noShot.textContent = "该站触发反爬或暂无截图——可点上方上传产品图";
      box.appendChild(noShot);
    }
    return;
  }
  if (sec === "sec-files") {
    var fBar = mk("div", "bm-toolbar");
    var upCtx = document.createElement("input");
    upCtx.type = "file"; upCtx.accept = ".md,.txt,.json,.csv,.yaml,.yml,.pdf";
    upCtx.style.display = "none";
    var upCtxBtn = mk("button", "btn acc"); upCtxBtn.type = "button";
    upCtxBtn.textContent = "+ 上传品牌文档 (.md/.txt/.json/.pdf)";
    upCtxBtn.onclick = function () { upCtx.click(); };
    upCtx.onchange = function () {
      var f = upCtx.files && upCtx.files[0];
      if (!f) return;
      var isPdf = /\.pdf$/i.test(f.name);
      var rd = new FileReader();
      rd.onload = function () {
        var payload = isPdf
          ? { filename: f.name, data_url: String(rd.result || "") }
          : { filename: f.name, content: String(rd.result || "") };
        saveBrandEdit(b, { add_context_file: payload }, "已关联品牌文档 " + f.name);
      };
      if (isPdf) rd.readAsDataURL(f);
      else rd.readAsText(f);
    };

    var upAud = document.createElement("input");
    upAud.type = "file"; upAud.accept = ".mp3,.wav,.m4a,.aac,.ogg"; upAud.style.display = "none";
    var upAudBtn = mk("button", "btn"); upAudBtn.type = "button";
    upAudBtn.textContent = "+ 上传品牌音效/BGM";
    upAudBtn.onclick = function () { upAud.click(); };
    upAud.onchange = function () {
      var f = upAud.files && upAud.files[0];
      if (!f) return;
      var rd = new FileReader();
      rd.onload = function () {
        saveBrandEdit(b, { upload_audio: {
          filename: f.name, kind: "sonic_logo", data_url: String(rd.result || ""),
        } }, "已上传品牌音频 " + f.name);
      };
      rd.readAsDataURL(f);
    };

    var newNoteBtn = mk("button", "btn"); newNoteBtn.type = "button";
    newNoteBtn.textContent = "+ 新建文本规范";
    newNoteBtn.onclick = function () {
      var wrap = box.querySelector(".bm-note-draft");
      if (wrap) { wrap.remove(); return; }
      wrap = mk("div", "bm-voice-form bm-note-draft");
      wrap.innerHTML = '<div class="bm-vf-row">' +
        '<div class="bm-vf-field"><span>文件名</span><input id="bm-cf-name" value="brand-guide.md"></div>' +
        '<div class="bm-vf-field"><span>内容正文（将注入 AI 上下文）</span><textarea id="bm-cf-body" rows="3" placeholder="输入品牌禁止项、产品卖点或口播禁忌…"></textarea></div></div>' +
        '<div class="bm-toolbar" style="margin:0"><button class="btn acc" id="bm-cf-save" type="button">保存文档</button></div>';
      fBar.insertAdjacentElement("afterend", wrap);
      wrap.querySelector("#bm-cf-save").onclick = function () {
        var fn = ($("bm-cf-name") ? $("bm-cf-name").value : "").trim() || "guide.md";
        var bd = $("bm-cf-body") ? $("bm-cf-body").value : "";
        saveBrandEdit(b, { add_context_file: { filename: fn, content: bd } }, "已新建参考文档 " + fn);
      };
    };

    fBar.appendChild(upCtx); fBar.appendChild(upCtxBtn);
    fBar.appendChild(newNoteBtn);
    fBar.appendChild(upAud); fBar.appendChild(upAudBtn);
    box.appendChild(fBar);

    var ctxSec = mk("div", "brand-sec"); ctxSec.textContent = "Context Docs（品牌上下文文档）";
    box.appendChild(ctxSec);
    var ctxBox = mk("div", "bm-file-group");
    if (!(b.context_files || []).length) {
      var emD = mk("div", "sb-empty"); emD.textContent = "尚未关联品牌参考文档（支持 PDF/MD/TXT）";
      ctxBox.appendChild(emD);
    }
    var docPreview = mk("pre", "bm-doc-preview");
    docPreview.style.display = "none";
    (b.context_files || []).forEach(function (cf) {
      var row = mk("div", "bm-file-row");
      var openBtn = mk("button", "bm-file-open"); openBtn.type = "button";
      openBtn.innerHTML = "📄 <b>" + escHtml(cf.name) + "</b> <span>" +
        escHtml(cf.excerpt || "") + "</span>";
      openBtn.onclick = function () {
        api("/api/brand-file/" + encodeURIComponent(id) + "?path=" + encodeURIComponent(cf.path || ""))
          .then(function (r) {
            docPreview.style.display = "block";
            docPreview.textContent = "【" + cf.name + "】\n" + (r.text || "（空文件）");
          }).catch(function (e) { log("读取文档失败：" + e.message, "err"); });
      };
      var rm = mk("button", "bm-col-del"); rm.type = "button"; rm.textContent = "×";
      rm.onclick = function () { saveBrandEdit(b, { remove_context_file: cf.path }, "已移除参考文档"); };
      row.appendChild(openBtn);
      row.appendChild(rm);
      ctxBox.appendChild(row);
    });
    box.appendChild(ctxBox);
    box.appendChild(docPreview);

    if ((b.audio_files || []).length) {
      var audSec = mk("div", "brand-sec"); audSec.textContent = "Audio Marks（品牌音频）";
      box.appendChild(audSec);
      (b.audio_files || []).forEach(function (af) {
        var ar = mk("div", "bm-file-row");
        ar.innerHTML = "<span>🔊 " + escHtml(af.name) + " (" + escHtml(af.kind || "audio") + ")</span>" +
          '<audio controls src="/api/brand-file/' + encodeURIComponent(id) +
          "?raw=1&path=" + encodeURIComponent(af.path || "") + '" style="height:26px"></audio>';
        var rm = mk("button", "bm-col-del"); rm.type = "button"; rm.textContent = "×";
        rm.onclick = function () { saveBrandEdit(b, { remove_audio: af.path }, "已移除音频"); };
        ar.appendChild(rm);
        box.appendChild(ar);
      });
    }

    var rawSec = mk("div", "brand-sec"); rawSec.textContent = "Media & Generated Assets（生成与媒体资产）";
    box.appendChild(rawSec);
    var fList = mk("div", "brand-ctx");
    (b._files || []).forEach(function (f) {
      var row = document.createElement("div");
      row.innerHTML = "<span>" + escHtml(f.path) + "</span><code>" +
        (f.bytes / 1024).toFixed(1) + " KB</code>";
      fList.appendChild(row);
    });
    box.appendChild(fList);
    return;
  }
  if (sec === "sec-rules") {
    var vObj = (typeof b.voice === "object" && b.voice) ? b.voice : {};
    var rObj = (typeof b.rules === "object" && b.rules) ? b.rules : {};
    var dosArr = rObj.dos || b.rules_txt || [];
    var dontsArr = rObj.donts || [];
    var form = mk("div", "bm-voice-form");
    form.innerHTML =
      '<div class="bm-vf-field"><span>品牌简介 Description</span>' +
      '<input id="bm-v-desc" value="' + escHtml(b.description || "") + '"></div>' +
      '<div class="bm-vf-row">' +
      '<div class="bm-vf-field"><span>表达语调 Tone</span>' +
      '<input id="bm-v-tone" value="' + escHtml(vObj.tone || "") + '"></div>' +
      '<div class="bm-vf-field"><span>核心标语 Tagline</span>' +
      '<input id="bm-v-tag" value="' + escHtml(vObj.tagline || "") + '"></div></div>' +
      '<div class="bm-vf-row">' +
      '<div class="bm-vf-field"><span>推荐用词 Say（每行一条）</span>' +
      '<textarea id="bm-v-say" rows="3">' + escHtml((vObj.say || []).join("\n")) + "</textarea></div>" +
      '<div class="bm-vf-field"><span>避用表达 Never Say（每行一条）</span>' +
      '<textarea id="bm-v-never" rows="3">' + escHtml((vObj.never_say || []).join("\n")) + "</textarea></div></div>" +
      '<div class="bm-vf-row">' +
      '<div class="bm-vf-field"><span>规则 Do\'s（每行一条）</span>' +
      '<textarea id="bm-v-dos" rows="3">' + escHtml(dosArr.join("\n")) + "</textarea></div>" +
      '<div class="bm-vf-field"><span>禁忌 Don\'ts（每行一条）</span>' +
      '<textarea id="bm-v-donts" rows="3">' + escHtml(dontsArr.join("\n")) + "</textarea></div></div>" +
      '<div class="bm-toolbar" style="margin:0"><button class="btn acc" id="bm-v-save" type="button">保存 Voice & Rules</button></div>';
    box.appendChild(form);
    var splitLines = function (id_) {
      var el = document.getElementById(id_);
      return el ? String(el.value || "").split("\n").map(function (s) { return s.trim(); }).filter(Boolean) : [];
    };
    var saveBtn = form.querySelector("#bm-v-save");
    if (saveBtn) saveBtn.onclick = function () {
      saveBrandEdit(b, {
        description: ($("bm-v-desc") ? $("bm-v-desc").value : "").trim(),
        voice: {
          tone: ($("bm-v-tone") ? $("bm-v-tone").value : "").trim(),
          tagline: ($("bm-v-tag") ? $("bm-v-tag").value : "").trim(),
          say: splitLines("bm-v-say"),
          never_say: splitLines("bm-v-never"),
        },
        rules: {
          dos: splitLines("bm-v-dos"),
          donts: splitLines("bm-v-donts"),
        },
      }, "已保存品牌 Voice 与规则");
    };
    var srcs = mk("div", "brand-rules");
    srcs.innerHTML = (b.sources || []).map(function (s) {
      return '<div class="brand-rule src"><b>⌘</b>' + escHtml(s.what) +
        (s.url ? " <code>" + escHtml(String(s.url).slice(0, 60)) + "</code>" : "") + "</div>";
    }).join("");
    box.appendChild(srcs);
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
