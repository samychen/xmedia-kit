import { renderInspector } from "./inspector.js";
import { recompose } from "./status.js";
import { api } from "../shared/api-client.js";
import { seekTo } from "../preview/driver.js";
import { escHtml, log, $ } from "../shared/dom.js";
import { renderSceneList, selectScene } from "./scenes.js";
import { renderTimeline } from "../timeline/render.js";
import { askOpen } from "../agent/ask.js";
import { watch } from "../shared/store.js";

var curRightTab = "insp";
var selectedDomEl = null;
var collapsedMap = new WeakMap();
var saveTimer = null;

function getPvDoc() {
  var pv = $("pv");
  try {
    return pv && pv.contentDocument;
  } catch (e) {
    return null;
  }
}

function getActiveSceneIdx() {
  var scenes = (state.spec && state.spec.scenes) || [];
  if (selIdx >= 0 && selIdx < scenes.length) return selIdx;
  return scenes.length ? 0 : -1;
}

function ensureOverlay(doc, id, dashed) {
  if (!doc || !doc.body) return null;
  var box = doc.getElementById(id);
  if (!box) {
    box = doc.createElement("div");
    box.id = id;
    box.style.cssText =
      "position:fixed;pointer-events:none;z-index:99999;border-radius:3px;" +
      "transition:all .04s ease-out;display:none;";
    if (!dashed) {
      ["tl:-4px,-4px", "tr:auto,-4px,-4px", "bl:-4px,auto,auto,-4px", "br:auto,-4px,auto,-4px"].forEach(function (pos, i) {
        var dot = doc.createElement("span");
        var st = "position:absolute;width:7px;height:7px;border-radius:50%;background:#fff;border:1.5px solid #38bdf8;";
        if (i === 0) st += "left:-4px;top:-4px;";
        else if (i === 1) st += "right:-4px;top:-4px;";
        else if (i === 2) st += "left:-4px;bottom:-4px;";
        else st += "right:-4px;bottom:-4px;";
        dot.style.cssText = st;
        box.appendChild(dot);
      });
      var rot = doc.createElement("div");
      rot.id = "__hf_elem_rot";
      rot.title = "拖动旋转元素";
      rot.textContent = "↻";
      rot.style.cssText =
        "position:absolute;left:50%;bottom:-26px;transform:translateX(-50%);" +
        "width:18px;height:18px;border-radius:50%;background:#181c22;color:#3ecf8e;" +
        "border:1.5px solid #3ecf8e;font-size:11px;line-height:16px;text-align:center;" +
        "cursor:grab;pointer-events:auto;user-select:none;box-shadow:0 2px 6px rgba(0,0,0,.5);";
      rot.addEventListener("pointerdown", function (ev) {
        ev.preventDefault();
        ev.stopPropagation();
        if (!selectedDomEl || !doc.defaultView) return;
        var rect = selectedDomEl.getBoundingClientRect();
        var cx = rect.left + rect.width / 2;
        var cy = rect.top + rect.height / 2;
        rot.style.cursor = "grabbing";
        function onMove(mv) {
          var rad = Math.atan2(mv.clientY - cy, mv.clientX - cx);
          var deg = Math.round((rad * 180 / Math.PI) - 90);
          if (mv.shiftKey) deg = Math.round(deg / 15) * 15;
          var prevT = (selectedDomEl.style.transform || "").replace(/rotate\([^)]*\)/g, "").trim();
          selectedDomEl.style.transform = (prevT ? prevT + " " : "") + "rotate(" + deg + "deg)";
          if ($("efb-rot")) $("efb-rot").textContent = "↻ " + deg + "°";
          positionBox(doc, "__hf_elem_sel", selectedDomEl, false);
        }
        function onUp() {
          rot.style.cursor = "grab";
          doc.removeEventListener("pointermove", onMove);
          doc.removeEventListener("pointerup", onUp);
          syncHtmlSceneToSpec();
        }
        doc.addEventListener("pointermove", onMove);
        doc.addEventListener("pointerup", onUp);
      });
      box.appendChild(rot);
    }
    doc.body.appendChild(box);
  }
  box.style.border = dashed
    ? "1.5px dashed rgba(56,189,248,0.85)"
    : "1.5px solid #38bdf8";
  box.style.boxShadow = dashed
    ? "none"
    : "0 0 0 1px rgba(0,0,0,0.35)";
  return box;
}

function positionBox(doc, boxId, el, dashed) {
  var box = ensureOverlay(doc, boxId, dashed);
  if (!box) return;
  if (!el || !el.isConnected) {
    box.style.display = "none";
    return;
  }
  var r = el.getBoundingClientRect();
  if (r.width <= 0 && r.height <= 0) {
    box.style.display = "none";
    return;
  }
  box.style.left = r.left + "px";
  box.style.top = r.top + "px";
  box.style.width = r.width + "px";
  box.style.height = r.height + "px";
  box.style.display = "block";
}

function rgbToHex(rgb) {
  var m = String(rgb || "").match(/\d+/g);
  if (!m || m.length < 3) return "#ffffff";
  return "#" + m.slice(0, 3).map(function (x) {
    return ("0" + Math.max(0, Math.min(255, parseInt(x, 10))).toString(16)).slice(-2);
  }).join("");
}

function nodeLabel(el) {
  var tag = (el.tagName || "").toLowerCase();
  var cls = Array.from(el.classList || []).filter(function (c) {
    return c !== "anim" && c !== "anim-pop" && c !== "anim-left" &&
           c !== "anim-right" && c !== "anim-bar" && c !== "anim-draw" &&
           c !== "scene-html-root" && c !== "clip" && c !== "scene";
  });
  var directTxt = "";
  for (var i = 0; i < el.childNodes.length; i++) {
    if (el.childNodes[i].nodeType === 3) {
      directTxt += el.childNodes[i].textContent;
    }
  }
  directTxt = directTxt.replace(/\s+/g, " ").trim();
  if (directTxt) return directTxt.slice(0, 26);
  if (cls.length) return "." + cls[0];
  if (tag === "svg") return "SVG 矢量图形";
  return tag;
}

function showFloatBar(el) {
  var bar = $("elem-float-bar");
  var doc = getPvDoc();
  if (!bar || !el || !doc || !doc.defaultView) {
    if (bar) bar.style.display = "none";
    return;
  }
  var cs = doc.defaultView.getComputedStyle(el);
  var fs = Math.round(parseFloat(cs.fontSize) || 24);
  if ($("efb-tag")) $("efb-tag").textContent = nodeLabel(el);
  if ($("efb-fs-val")) $("efb-fs-val").textContent = String(fs);
  if ($("efb-color")) $("efb-color").value = rgbToHex(cs.color);
  var mRot = (el.style.transform || "").match(/rotate\((-?\d+(?:\.\d+)?)deg\)/);
  if ($("efb-rot")) $("efb-rot").textContent = "↻ " + (mRot ? Math.round(parseFloat(mRot[1])) : 0) + "°";
  bar.style.display = "flex";
}

function startInlineEdit(el) {
  var doc = getPvDoc();
  if (!el || !doc) return;
  if (el.children && el.children.length > 0 && el.tagName.toLowerCase() !== "span") {
    var hasNestedBlock = Array.from(el.children).some(function (c) {
      return c.tagName.toLowerCase() !== "br";
    });
    if (hasNestedBlock) {
      log("请选择具体的文本子节点后再编辑文字", "warn");
      return;
    }
  }
  el.setAttribute("contenteditable", "true");
  el.style.outline = "2px solid #3ecf8e";
  el.focus();
  function finish() {
    el.removeAttribute("contenteditable");
    el.style.outline = "";
    el.removeEventListener("blur", finish);
    el.removeEventListener("keydown", onKey);
    positionBox(doc, "__hf_elem_sel", el, false);
    showFloatBar(el);
    renderLayersPanel();
    syncHtmlSceneToSpec();
  }
  function onKey(ev) {
    if (ev.key === "Enter" && !ev.shiftKey) {
      ev.preventDefault();
      el.blur();
    } else if (ev.key === "Escape") {
      ev.preventDefault();
      el.blur();
    }
  }
  el.addEventListener("blur", finish);
  el.addEventListener("keydown", onKey);
}

export function clearElementSelection() {
  selectedDomEl = null;
  var doc = getPvDoc();
  if (doc) {
    var b1 = doc.getElementById("__hf_elem_sel");
    var b2 = doc.getElementById("__hf_elem_hover");
    if (b1) b1.style.display = "none";
    if (b2) b2.style.display = "none";
  }
  if ($("elem-float-bar")) $("elem-float-bar").style.display = "none";
}

export function selectDomElement(el, fromCanvas) {
  var doc = getPvDoc();
  if (!doc || !el) return;
  selectedDomEl = el;
  positionBox(doc, "__hf_elem_sel", el, false);
  showFloatBar(el);
  if (fromCanvas) {
    var scEl = el.closest && el.closest(".scene[id^='scene-']");
    if (scEl) {
      var idx = parseInt(scEl.id.replace("scene-", ""), 10);
      if (!isNaN(idx) && idx !== selIdx) {
        selectScene(idx, true);
      }
    }
  }
  renderLayersPanel();
}

function syncHtmlSceneToSpec() {
  var idx = getActiveSceneIdx();
  var doc = getPvDoc();
  if (idx < 0 || !doc || !state.spec || !state.spec.scenes[idx]) return;
  var sc = state.spec.scenes[idx];
  if (sc.type !== "html") return;
  var root = doc.querySelector("#scene-" + idx + " .scene-html-root");
  if (!root) return;
  sc.html = root.innerHTML;
  $("spec").value = JSON.stringify(state.spec, null, 2);
  clearTimeout(saveTimer);
  saveTimer = setTimeout(function () {
    api("/api/spec-save", { spec: state.spec }).catch(function () {});
  }, 450);
}

var curFilesDir = "";
var playingFile = "";

function stopPreviewAudio() {
  var box = $("files-preview");
  if (box) {
    box.querySelectorAll("audio,video").forEach(function (m) {
      if (m.pause) m.pause();
      m.removeAttribute("src");
    });
  }
  playingFile = "";
}

function showFilePreviewBox(m, kind) {
  var box = $("files-preview");
  if (!box) return;
  stopPreviewAudio();
  box.replaceChildren();
  var src = "/" + m.replace(/^\/+/, "");
  var head = document.createElement("div");
  head.style.cssText = "display:flex;align-items:center;justify-content:space-between;font-size:11px;color:var(--dim);gap:6px";
  var ttl = document.createElement("span");
  ttl.style.cssText = "overflow:hidden;text-overflow:ellipsis;white-space:nowrap;flex:1";
  ttl.textContent = m;
  head.appendChild(ttl);
  if (kind === "html") {
    var openBtn = document.createElement("button");
    openBtn.className = "mini"; openBtn.type = "button"; openBtn.textContent = "新窗打开";
    openBtn.onclick = function () { window.open(src, "_blank"); };
    head.appendChild(openBtn);
  }
  var close = document.createElement("button");
  close.className = "mini"; close.type = "button"; close.textContent = "×";
  close.onclick = function () { stopPreviewAudio(); box.style.display = "none"; renderFilesPanel(); };
  head.appendChild(close);
  box.appendChild(head);
  if (kind === "image") {
    var img = document.createElement("img");
    img.src = src;
    box.appendChild(img);
  } else if (kind === "video") {
    var vid = document.createElement("video");
    vid.src = src; vid.controls = true; vid.autoplay = true;
    box.appendChild(vid);
  } else if (kind === "audio") {
    var aud = document.createElement("audio");
    aud.src = src; aud.controls = true; aud.autoplay = true;
    aud.style.width = "100%";
    playingFile = m;
    aud.onplay = function () { playingFile = m; };
    aud.onended = function () { playingFile = ""; renderFilesPanel(); };
    box.appendChild(aud);
  } else {
    var pre = document.createElement("pre");
    pre.style.cssText = "margin:0;max-height:140px;overflow:auto;font:10.5px/1.45 ui-monospace,Menlo,monospace;color:var(--ink);background:var(--bg);padding:6px 8px;border-radius:6px;white-space:pre-wrap;word-break:break-all";
    pre.textContent = "加载中…";
    box.appendChild(pre);
    fetch(src).then(function (r) { return r.text(); }).then(function (t) {
      pre.textContent = t.slice(0, 4000) + (t.length > 4000 ? "\n…（已截断）" : "");
    }).catch(function () { pre.textContent = "无法读取文件内容"; });
  }
  box.style.display = "flex";
}

function attachMediaToScene(m, kind) {
  if (selIdx < 0 || !state.spec || !state.spec.scenes[selIdx]) {
    log("请先选中一个目标场景", "err");
    return;
  }
  var sc = state.spec.scenes[selIdx];
  if (kind === "audio") sc.narration = m;
  else if (kind === "video") sc.video = m;
  else sc.image = { src: m, mode: "corner" };
  $("spec").value = JSON.stringify(state.spec, null, 2);
  renderInspector(); renderSceneList(); renderTimeline();
  recompose();
}

function makeSceneMiniThumb(idx) {
  var doc = getPvDoc();
  var scNode = doc && doc.getElementById("scene-" + idx);
  if (!scNode) return null;
  var w = (state.width || 1920);
  var h = (state.height || 1080);
  var scale = Math.min(160 / w, 74 / h);
  var ifr = document.createElement("iframe");
  ifr.className = "comp-mini-frame";
  ifr.setAttribute("tabindex", "-1");
  ifr.onload = function () {
    try {
      var idoc = ifr.contentDocument;
      if (!idoc || !idoc.head || !idoc.body) return;
      doc.querySelectorAll("style").forEach(function (s) {
        idoc.head.appendChild(s.cloneNode(true));
      });
      var ov = idoc.createElement("style");
      ov.textContent =
        "html,body{margin:0;padding:0;overflow:hidden;background:#0f1117;}" +
        "#mini-wrap{width:" + w + "px;height:" + h + "px;transform:scale(" +
        scale.toFixed(4) + ");transform-origin:top left;position:relative;}" +
        "#mini-wrap .scene,#mini-wrap .scene *{opacity:1!important;visibility:visible!important;clip-path:none!important;}";
      idoc.head.appendChild(ov);
      var wrap = idoc.createElement("div");
      wrap.id = "mini-wrap";
      wrap.appendChild(scNode.cloneNode(true));
      idoc.body.appendChild(wrap);
    } catch (err) {
      if (err) return;
    }
  };
  return ifr;
}

export function renderFilesPanel() {
  var grid = $("files-grid");
  var crumb = $("files-crumb");
  if (!grid || !crumb) return;
  var q = (($("files-search") || {}).value || "").trim().toLowerCase();
  var allItems = (state.project_files && state.project_files.length)
    ? state.project_files.slice()
    : (state.media_files || []).slice();
  var scenes = (state.spec && state.spec.scenes) || [];

  crumb.replaceChildren();
  if (curFilesDir) {
    var back = document.createElement("button");
    back.type = "button";
    back.textContent = "< All files";
    back.onclick = function () { curFilesDir = ""; renderFilesPanel(); };
    crumb.appendChild(back);
    var sp = document.createElement("span");
    sp.textContent = "/ " + curFilesDir;
    crumb.appendChild(sp);
  } else {
    crumb.textContent = "All files (" + (allItems.length + (scenes.length ? 1 : 0)) + ")";
  }

  grid.replaceChildren();

  if (curFilesDir === "compositions") {
    scenes.forEach(function (sc, i) {
      var meta = (state.scenes || [])[i] || {};
      var lbl = sceneTabTitle(sc, i);
      if (q && (lbl + " " + (sc.type || "")).toLowerCase().indexOf(q) < 0) return;
      var card = document.createElement("div");
      card.className = "file-card" + (i === selIdx ? " playing" : "");
      var thumb = document.createElement("div");
      thumb.className = "file-thumb";
      var mini = makeSceneMiniThumb(i);
      if (mini) thumb.appendChild(mini);
      else {
        thumb.style.background = sc.bg || "#161820";
        thumb.textContent = "🎬";
      }
      card.appendChild(thumb);
      var metaEl = document.createElement("div");
      metaEl.className = "file-meta";
      metaEl.innerHTML =
        '<div class="file-name" title="' + escHtml(lbl) + '">scene-' + (i + 1) + '.html · ' + escHtml(lbl.slice(0, 14)) + '</div>' +
        '<div class="file-kind"><span>' + escHtml((sc.type || "text").toUpperCase()) + ' · ' + Number(sc.duration || 3).toFixed(1) + 's</span></div>';
      card.appendChild(metaEl);
      card.onclick = function () {
        selectScene(i, true);
        if (typeof meta.start === "number") seekTo(meta.start + Math.min(0.6, (meta.duration || 2) * 0.35));
        renderFilesPanel();
      };
      grid.appendChild(card);
    });
    if (!grid.children.length) {
      grid.innerHTML = '<div class="empty" style="grid-column:1/-1;padding:14px">暂无匹配场景</div>';
    }
    return;
  }

  var dirs = {};
  var entries = [];

  if (!curFilesDir && !q && scenes.length) {
    var compCard = document.createElement("div");
    compCard.className = "file-card";
    compCard.innerHTML =
      '<div class="file-thumb">📁</div>' +
      '<div class="file-meta"><div class="file-name">compositions</div>' +
      '<div class="file-kind"><span>Folder · ' + scenes.length + '</span></div></div>';
    compCard.onclick = function () { curFilesDir = "compositions"; renderFilesPanel(); };
    grid.appendChild(compCard);
  }

  allItems.forEach(function (item) {
    if (q) {
      if (item.toLowerCase().indexOf(q) >= 0) entries.push({ type: "file", path: item, name: item });
      return;
    }
    if (curFilesDir) {
      if (item.indexOf(curFilesDir + "/") === 0) {
        entries.push({ type: "file", path: item, name: item.slice(curFilesDir.length + 1) });
      }
      return;
    }
    var slash = item.indexOf("/");
    if (slash > 0) {
      var d = item.slice(0, slash);
      dirs[d] = (dirs[d] || 0) + 1;
    } else {
      entries.push({ type: "file", path: item, name: item });
    }
  });

  Object.keys(dirs).sort().forEach(function (d) {
    var card = document.createElement("div");
    card.className = "file-card";
    card.innerHTML =
      '<div class="file-thumb">📁</div>' +
      '<div class="file-meta"><div class="file-name">' + escHtml(d) + '</div>' +
      '<div class="file-kind"><span>Folder · ' + dirs[d] + '</span></div></div>';
    card.onclick = function () { curFilesDir = d; renderFilesPanel(); };
    grid.appendChild(card);
  });

  entries.slice(0, 60).forEach(function (it) {
    var p = it.path;
    var isAudio = /\.(mp3|wav|m4a|aac)$/i.test(p);
    var isVideo = /\.(mp4|mov|webm)$/i.test(p);
    var isImg = /\.(png|jpe?g|webp|svg|gif)$/i.test(p);
    var isHtml = /\.html?$/i.test(p);
    var kind = isAudio ? "audio" : isVideo ? "video" : isImg ? "image" : isHtml ? "html" : "file";

    var card = document.createElement("div");
    card.className = "file-card" + (playingFile === p ? " playing" : "");
    var thumb = document.createElement("div");
    thumb.className = "file-thumb";
    if (isImg) {
      thumb.innerHTML = '<img src="/' + escHtml(p.replace(/^\/+/, "")) + '" loading="lazy" alt="">';
    } else if (isAudio) {
      thumb.textContent = playingFile === p ? "⏸" : "🔊";
    } else if (isVideo) {
      thumb.textContent = "🎞️";
    } else if (isHtml) {
      var mini0 = makeSceneMiniThumb(0);
      if (mini0) thumb.appendChild(mini0);
      else thumb.textContent = "🌐";
    } else {
      thumb.textContent = p.endsWith(".json") || p.endsWith(".jsonl") ? "{ }" : "📄";
    }
    card.appendChild(thumb);

    var meta = document.createElement("div");
    meta.className = "file-meta";
    meta.innerHTML = '<div class="file-name" title="' + escHtml(p) + '">' + escHtml(it.name) + '</div>';
    var krow = document.createElement("div");
    krow.className = "file-kind";
    var extLbl = (p.split(".").pop() || kind).toUpperCase();
    krow.innerHTML = '<span>' + escHtml(extLbl) + '</span>';

    if (isAudio || isVideo || isImg) {
      var act = document.createElement("button");
      act.className = "file-act"; act.type = "button";
      act.textContent = isAudio ? "设旁白" : isVideo ? "设视频" : "设配图";
      act.onclick = function (ev) { ev.stopPropagation(); attachMediaToScene(p, kind); };
      krow.appendChild(act);
    }
    meta.appendChild(krow);
    card.appendChild(meta);

    card.onclick = function () {
      if (isAudio) {
        if (playingFile === p) {
          stopPreviewAudio();
          if ($("files-preview")) $("files-preview").style.display = "none";
          renderFilesPanel();
        } else {
          showFilePreviewBox(p, "audio");
          renderFilesPanel();
        }
        return;
      }
      showFilePreviewBox(p, kind);
    };
    grid.appendChild(card);
  });

  if (!Object.keys(dirs).length && !entries.length) {
    grid.innerHTML = '<div class="empty" style="grid-column:1/-1;padding:14px">暂无匹配文件</div>';
  }
}

export function renderLayersPanel() {
  var tree = $("layers-tree");
  if (!tree) return;
  var q = (($("layers-search") || {}).value || "").trim().toLowerCase();

  tree.replaceChildren();
  var scenes = (state.spec && state.spec.scenes) || [];
  if (!scenes.length) {
    tree.innerHTML = '<div class="empty" style="padding:14px">暂无场景图层</div>';
    return;
  }

  var idx = getActiveSceneIdx();
  var bar = document.createElement("div");
  bar.className = "dom-scene-bar";
  var sel = document.createElement("select");
  scenes.forEach(function (sc, i) {
    var opt = document.createElement("option");
    opt.value = String(i);
    var t = sc.text || sc.title || sc.quote || sc.value || sc.type || "scene";
    opt.textContent = "#" + (i + 1) + " · " + String(t).replace(/<[^>]+>/g, "").slice(0, 18);
    if (i === idx) opt.selected = true;
    sel.appendChild(opt);
  });
  sel.onchange = function () {
    var ni = parseInt(sel.value, 10);
    selectScene(ni, true);
    var meta = (state.scenes || [])[ni] || {};
    if (typeof meta.start === "number") seekTo(meta.start + 0.35);
    clearElementSelection();
    setTimeout(renderLayersPanel, 80);
  };
  bar.appendChild(sel);
  tree.appendChild(bar);

  var doc = getPvDoc();
  var scNode = doc && doc.getElementById("scene-" + idx);
  if (!scNode) {
    tree.insertAdjacentHTML("beforeend", '<div class="empty" style="padding:10px">预览画布加载中…</div>');
    return;
  }
  var rootEl = scNode.querySelector(".scene-html-root") || scNode;
  var wrap = document.createElement("div");
  wrap.className = "dom-tree";

  function isMeaningful(el) {
    var tag = el.tagName.toLowerCase();
    if (tag === "script" || tag === "style" || tag === "br" || tag === "hr") return false;
    if (el.classList.contains("scene-caption") || el.classList.contains("hairline") ||
        el.classList.contains("invert-block")) return false;
    var hasText = (el.textContent || "").replace(/\s+/g, "").length > 0;
    var isMedia = tag === "svg" || tag === "img" || tag === "video";
    if (!hasText && !isMedia) return false;
    return true;
  }

  function unwrapChain(el) {
    // 压平纯布局空壳（只有 1 个有意义子节点且自身无直接文本）
    var cur = el;
    for (var step = 0; step < 3; step++) {
      var directTxt = "";
      for (var i = 0; i < cur.childNodes.length; i++) {
        if (cur.childNodes[i].nodeType === 3) directTxt += cur.childNodes[i].textContent;
      }
      if (directTxt.trim()) break;
      var kids = Array.from(cur.children || []).filter(isMeaningful);
      if (kids.length === 1 && cur.tagName.toLowerCase() !== "svg") {
        cur = kids[0];
      } else {
        break;
      }
    }
    return cur;
  }

  function buildNode(rawEl, depth) {
    if (!rawEl || rawEl.nodeType !== 1 || !isMeaningful(rawEl)) return;
    var el = unwrapChain(rawEl);
    var tag = el.tagName.toLowerCase();
    var kids = Array.from(el.children || []).filter(isMeaningful);
    var label = nodeLabel(el);
    var match = !q || label.toLowerCase().indexOf(q) >= 0 || tag.indexOf(q) >= 0;

    if (match) {
      var row = document.createElement("div");
      row.className = "dom-row" + (selectedDomEl === el ? " sel" : "");
      row.style.paddingLeft = (6 + depth * 12) + "px";

      var tog = document.createElement("span");
      tog.className = "dom-toggle";
      var isCol = !!collapsedMap.get(el);
      tog.textContent = kids.length ? (isCol ? "▸" : "▾") : "";
      tog.onclick = function (ev) {
        ev.stopPropagation();
        if (!kids.length) return;
        collapsedMap.set(el, !isCol);
        renderLayersPanel();
      };
      row.appendChild(tog);

      var ico = document.createElement("span");
      var isSvg = tag === "svg" || tag === "path" || tag === "circle" || tag === "rect";
      var isTxt = !kids.length && !isSvg;
      ico.className = "dom-ico" + (isSvg ? " svg" : isTxt ? " txt" : "");
      ico.textContent = isSvg ? "◇" : isTxt ? "T" : "□";
      row.appendChild(ico);

      var nm = document.createElement("span");
      nm.className = "dom-name";
      nm.textContent = label;
      row.appendChild(nm);

      if (kids.length) {
        var cnt = document.createElement("span");
        cnt.className = "dom-cnt";
        cnt.textContent = String(kids.length);
        row.appendChild(cnt);
      }

      row.onmouseenter = function () {
        positionBox(doc, "__hf_elem_hover", el, true);
      };
      row.onmouseleave = function () {
        var b = doc.getElementById("__hf_elem_hover");
        if (b) b.style.display = "none";
      };
      row.onclick = function () {
        var meta = (state.scenes || [])[idx] || {};
        if (typeof meta.start === "number" && (lastTime < meta.start || lastTime > meta.start + (meta.duration || 3))) {
          seekTo(meta.start + Math.min(0.8, (meta.duration || 2) * 0.4));
        }
        selectDomElement(el, false);
      };
      wrap.appendChild(row);
    }

    if (!collapsedMap.get(el)) {
      var nextDepth = depth + 1;
      if (tag === "svg") return; // SVG 内部 path 不全铺开以保持清晰
      kids.forEach(function (c) { buildNode(c, nextDepth); });
    }
  }

  Array.from(rootEl.children || []).forEach(function (c) {
    if (!c.classList.contains("scene-caption")) buildNode(c, 0);
  });
  if (!wrap.children.length) buildNode(rootEl, 0);
  tree.appendChild(wrap);
}

export function bindPreviewSelection() {
  var doc = getPvDoc();
  if (!doc || doc.__hfBound) return;
  doc.__hfBound = true;
  doc.addEventListener("click", function (ev) {
    if (!$("pvtool-select") || !$("pvtool-select").classList.contains("on")) return;
    var t = ev.target;
    if (!t || t.nodeType !== 1 || t === doc.body || t === doc.documentElement) return;
    if (t.id === "__hf_elem_rot" || t.closest("#__hf_elem_sel")) return;
    var sc = t.closest && t.closest(".scene[id^='scene-']");
    if (!sc) return;
    selectDomElement(t, true);
  }, false);
  doc.addEventListener("dblclick", function (ev) {
    if (!$("pvtool-select") || !$("pvtool-select").classList.contains("on")) return;
    var t = ev.target;
    if (!t || t.nodeType !== 1 || !t.closest(".scene[id^='scene-']")) return;
    selectDomElement(t, true);
    startInlineEdit(t);
  }, false);
}

export function renderSettingsPanel() {
  var sp = state.spec;
  if (!sp) return;
  var comp = sp.composition || {};
  var tr = sp.transitions || {};
  if ($("set-format")) $("set-format").value = comp.format || "16:9";
  if ($("set-fps")) $("set-fps").value = String(comp.fps || 30);
  if ($("set-trans")) $("set-trans").value = tr.type || "fade";
  if ($("set-trans-dur")) $("set-trans-dur").value = String(tr.duration !== undefined ? tr.duration : 0.45);
  if ($("set-captions")) $("set-captions").checked = !sp.captions || sp.captions.enabled !== false;
  if ($("set-sfx-on")) $("set-sfx-on").checked = !sp.sfx || sp.sfx.enabled !== false;
}

export function switchRightPanel(name) {
  curRightTab = name;
  var tabs = { insp: "rtab-insp", layers: "rtab-layers", files: "rtab-files", settings: "rtab-settings" };
  var panels = { insp: "rpanel-insp", layers: "rpanel-layers", files: "rpanel-files", settings: "rpanel-settings" };
  for (var k in tabs) {
    if ($(tabs[k])) $(tabs[k]).classList.toggle("active", k === name);
    if ($(panels[k])) $(panels[k]).style.display = k === name ? "flex" : "none";
  }
  if (name === "layers") renderLayersPanel();
  if (name === "files") renderFilesPanel();
  if (name === "settings") renderSettingsPanel();
}

export function bindRightTabs() {
  if ($("rtab-insp")) $("rtab-insp").onclick = function () { switchRightPanel("insp"); };
  if ($("rtab-layers")) $("rtab-layers").onclick = function () { switchRightPanel("layers"); };
  if ($("rtab-files")) $("rtab-files").onclick = function () { switchRightPanel("files"); };
  if ($("rtab-settings")) $("rtab-settings").onclick = function () { switchRightPanel("settings"); };
  if ($("rail-assets")) $("rail-assets").onclick = function () { switchRightPanel("files"); };
  if ($("rail-insp")) $("rail-insp").onclick = function () { switchRightPanel("insp"); };
  if ($("rail-settings")) $("rail-settings").onclick = function () { switchRightPanel("settings"); };
  if ($("rail-layers")) $("rail-layers").onclick = function () { switchRightPanel("layers"); };
  if ($("layers-search")) $("layers-search").oninput = renderLayersPanel;
  if ($("files-search")) $("files-search").oninput = renderFilesPanel;

  if ($("pv")) $("pv").addEventListener("load", function () {
    bindPreviewSelection();
    clearElementSelection();
    setTimeout(function () {
      renderLayersPanel();
      if (curRightTab === "files") renderFilesPanel();
    }, 120);
  });

  if ($("efb-close")) $("efb-close").onclick = function () {
    clearElementSelection();
    renderLayersPanel();
  };
  function adjustFontSize(delta) {
    var doc = getPvDoc();
    if (!selectedDomEl || !doc || !doc.defaultView) return;
    var cur = Math.round(parseFloat(doc.defaultView.getComputedStyle(selectedDomEl).fontSize) || 24);
    var next = Math.max(10, Math.min(200, cur + delta));
    selectedDomEl.style.fontSize = next + "px";
    if ($("efb-fs-val")) $("efb-fs-val").textContent = String(next);
    positionBox(doc, "__hf_elem_sel", selectedDomEl, false);
    syncHtmlSceneToSpec();
  }
  if ($("efb-fs-dec")) $("efb-fs-dec").onclick = function () { adjustFontSize(-4); };
  if ($("efb-fs-inc")) $("efb-fs-inc").onclick = function () { adjustFontSize(4); };
  if ($("efb-color")) $("efb-color").oninput = function () {
    if (!selectedDomEl) return;
    selectedDomEl.style.color = $("efb-color").value;
    syncHtmlSceneToSpec();
  };
  if ($("efb-rot")) $("efb-rot").onclick = function () {
    var doc = getPvDoc();
    if (!selectedDomEl || !doc) return;
    selectedDomEl.style.transform = (selectedDomEl.style.transform || "").replace(/rotate\([^)]*\)/g, "").trim();
    $("efb-rot").textContent = "↻ 0°";
    positionBox(doc, "__hf_elem_sel", selectedDomEl, false);
    syncHtmlSceneToSpec();
  };
  if ($("efb-edit-txt")) $("efb-edit-txt").onclick = function () {
    if (selectedDomEl) startInlineEdit(selectedDomEl);
  };
  if ($("efb-ask-ai")) $("efb-ask-ai").onclick = function () {
    if (!selectedDomEl) return;
    var lbl = nodeLabel(selectedDomEl);
    var idx = getActiveSceneIdx();
    askOpen(true);
    var inp = $("ask-input");
    if (inp) {
      inp.value = "把第 " + (idx + 1) + " 镜里的元素「" + lbl + "」";
      inp.focus();
    }
  };

  if ($("set-apply")) $("set-apply").onclick = function () {
    if (!state.spec) return;
    state.spec.composition = Object.assign({}, state.spec.composition || {}, {
      format: $("set-format").value,
      fps: parseInt($("set-fps").value, 10) || 30
    });
    state.spec.transitions = {
      type: $("set-trans").value,
      duration: Math.max(0, parseFloat($("set-trans-dur").value) || 0)
    };
    state.spec.captions = Object.assign({}, state.spec.captions || {}, {
      enabled: $("set-captions").checked
    });
    state.spec.sfx = Object.assign({}, state.spec.sfx || {}, {
      enabled: $("set-sfx-on").checked
    });
    if ($("sfx")) $("sfx").checked = $("set-sfx-on").checked;
    $("spec").value = JSON.stringify(state.spec, null, 2);
    log("已更新合成设置，重组合成…");
    recompose();
  };
}

function sceneTabTitle(sc, i) {
  var direct = sc.name || sc.role || sc.kicker || sc.title || sc.text || sc.quote || sc.value;
  if (direct) {
    var s = String(direct).replace(/<[^>]+>/g, " ").replace(/\s+/g, " ").trim();
    if (s) return s.slice(0, 12);
  }
  if (sc.html) {
    try {
      var doc = new DOMParser().parseFromString(sc.html, "text/html");
      var hit = doc.body.querySelector("h1,h2,h3,.kicker,.title,.headline,.h");
      var txt = ((hit ? hit.textContent : doc.body.textContent) || "").replace(/\s+/g, " ").trim();
      if (txt) return txt.slice(0, 12);
    } catch (e) {
      if (e) return "镜 " + (i + 1);
    }
  }
  var narr = sc.narration_text || (typeof sc.narration === "string" && !/\.(mp3|wav|m4a)$/i.test(sc.narration) ? sc.narration : "");
  if (narr) return String(narr).trim().slice(0, 10);
  return (sc.type && sc.type !== "html" ? sc.type : "镜 " + (i + 1));
}

export function renderPvSceneTabs() {
  var box = $("pv-scene-tabs");
  var mainBtn = $("pv-tab-main");
  if (!box) return;
  box.replaceChildren();
  var scenes = (state.spec && state.spec.scenes) || [];
  if (mainBtn) {
    mainBtn.classList.toggle("active", selIdx < 0);
    mainBtn.onclick = function () {
      selectScene(-1, true);
      seekTo(0);
      renderPvSceneTabs();
    };
  }
  scenes.forEach(function (sc, i) {
    var meta = (state.scenes || [])[i] || {};
    var lbl = sceneTabTitle(sc, i);
    var btn = document.createElement("button");
    btn.className = "pv-tab" + (i === selIdx ? " active" : "");
    btn.type = "button";
    btn.title = "第 " + (i + 1) + " 镜 (" + (sc.type || "text") + ")";
    btn.textContent = lbl;
    btn.onclick = function () {
      selectScene(i, true);
      if (typeof meta.start === "number") {
        seekTo(meta.start + Math.min(0.6, (meta.duration || 2) * 0.35));
      }
      renderPvSceneTabs();
    };
    box.appendChild(btn);
  });
}

export function getCurRightTab() { return curRightTab; }

watch("selIdx", function () {
  clearElementSelection();
  renderLayersPanel();
  renderPvSceneTabs();
});
