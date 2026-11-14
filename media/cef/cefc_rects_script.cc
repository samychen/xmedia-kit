// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// MakeRectsScript 的定义：独立文件，避免往 cef_osr_client.cpp（861 行）
// 里做增量手术。声明在 cef_osr_client.h（namespace xmedia::cef）。

#include "media/cef/cef_osr_client.h"

namespace xmedia {
namespace cef {

// 页面数据注入（MC MakeUpdateScript 对齐）：window.update(d) 契约
//（caspar.update 兜底），页面经回调消费 data JSON。
std::string MakeUpdateScript(const std::string& json_utf8) {
  return std::string(
             "(function(){\n"
             "  var d = ") +
         json_utf8 +
         ";\n"
         "  try {\n"
         "    if (typeof window.update === 'function') {\n"
         "      window.update(d);\n"
         "      return;\n"
         "    }\n"
         "    console.warn('[bake] window.update missing');\n"
         "  } catch (e) {\n"
         "    console.warn('[bake] data inject failed', e);\n"
         "  }\n"
         "})();\n";
}

// 注入前校验（安全门）：拒绝非 JSON / 非对象数组根——原始拼接进
// ExecuteJavaScript 等于脚本注入。无 jsoncpp，用最小状态机：根必须是
// { 或 [，字符串/转义感知的括号配平 + 结尾无垃圾。非完整 JSON 解析器，
// 语义错误交给页面侧 window.update 自然失败。
bool ValidateHtmlDataJson(const std::string& json_utf8) {
  size_t i = 0;
  while (i < json_utf8.size() &&
         (json_utf8[i] == ' ' || json_utf8[i] == '\t' ||
          json_utf8[i] == '\r' || json_utf8[i] == '\n')) {
    ++i;
  }
  if (i >= json_utf8.size() ||
      (json_utf8[i] != '{' && json_utf8[i] != '[')) {
    return false;
  }
  int depth = 0;
  bool in_string = false;
  bool escaped = false;
  bool closed = false;
  for (; i < json_utf8.size(); ++i) {
    const char c = json_utf8[i];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (c == '\\') {
        escaped = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      --depth;
      if (depth == 0) {
        closed = true;
      } else if (depth < 0) {
        return false;
      }
    }
  }
  return closed && !in_string;
}

// Bake seek 时钟（对齐 MC HtmlLayerProducer）：页面实现 window.seek(t_ms)
// 时调用之并让采集脚本在新 t 重报 rects；页面未实现时一次性上报
// mc.html.error:no_seek，bake 侧据此回退 live 墙钟绘制。
std::string MakeSeekScript(int64_t t_ms) {
  return std::string(
             "(function(t){\n"
             "  // 页面未加载完（采集脚本 OnLoadEnd 注入，故以它的在位标记为\n"
             "  // 就绪信号）：静默跳过，后续 seek 重试。大页面（内联 GSAP 等\n"
             "  // 大脚本解析久）的首个 seek 会早于脚本执行——误判 no_seek 会\n"
             "  // 被 bake 侧粘性短路，GSAP 从此永不推进（实测：可见性切换、\n"
             "  // 画面停在初始 DOM 状态）。\n"
             "  if (!window.__mcRectsCollector) { return; }\n"
             "  if (window.__timelines || window.__hf) {\n"
             "    var tt = t / 1000;\n"
             "    if (window.__hf && typeof window.__hf.seek === 'function') {\n"
             "      try { window.__hf.seek(tt); } catch (e) {}\n"
             "    }\n"
             "    try {\n"
             "      Object.values(window.__timelines || {}).forEach("
             "function (tl) {\n"
             "        try {\n"
             "          if (typeof tl.pause === 'function') { tl.pause(); }\n"
             "          if (typeof tl.totalTime === 'function') {\n"
             "            tl.totalTime(tt + 1e-3, true);\n"
             "          } else if (typeof tl.seek === 'function') {\n"
             "            tl.seek(tt);\n"
             "          }\n"
             "        } catch (e) {}\n"
             "      });\n"
             "    } catch (e) {}\n"
             "    // CSS 可见性同步：注册表 totalTime 只推 GSAP 状态，不触碰\n"
             "    // 内联 visibility——warmup 时 __mcApplyTimeline(0) 给未生效\n"
             "    // 元素打的 hidden 若不逐帧解除，元素 opacity 到 1 也不渲染。\n"
             "    if (typeof window.__mcApplyTimeline === 'function') {\n"
             "      try { window.__mcApplyTimeline(t); } catch (e) {}\n"
             "    }\n"
             "    if (typeof window.__mcReportRects === 'function') {\n"
             "      try { window.__mcReportRects(t); } catch (e) {}\n"
             "    }\n"
             "    if (window.cefQuery) {\n"
             "      var done = function () {\n"
             "        window.cefQuery({request:'mc.html.seekDone:' + t,"
             "persistent:false,onSuccess:function(){},"
             "onFailure:function(){}});\n"
             "      };\n"
             "      if (typeof window.__hfWaitForSeekCompletion ==="
             " 'function') {\n"
             "        Promise.resolve("
             "window.__hfWaitForSeekCompletion()).then(done, done);\n"
             "      } else { done(); }\n"
             "    }\n"
             "    return;\n"
             "  }\n"
             "  if (typeof window.seek === 'function') {\n"
             "    window.seek(t);\n"
             "    if (typeof window.__mcReportRects === 'function') {\n"
             "      try { window.__mcReportRects(t); } catch (e) {}\n"
             "    }\n"
             "    return;\n"
             "  }\n"
             "  if (!window.__mcBakeSeekWarned) {\n"
             "    window.__mcBakeSeekWarned = true;\n"
             "    console.warn('[bake] window.seek missing');\n"
             "    if (window.cefQuery) {\n"
             "      window.cefQuery({request:'mc.html.error:no_seek',"
             "persistent:false,onSuccess:function(){},"
             "onFailure:function(){}});\n"
             "    }\n"
             "  }\n"
             "})(") +
         std::to_string(t_ms) + ");\n";
}

std::string MakeRectsScript() {
  return R"js((function(){
  if (window.__mcRectsCollector) { return; }
  window.__mcRectsCollector = true;
  var SEL = '[data-start],[data-duration],[data-z]';
  function num(v) {
    if (v === null) { return null; }
    var n = Number(v);
    return isFinite(n) ? n : null;
  }
  // 时间属性单位归一：HF 契约页的 data-start/duration 等是秒；旧契约页
  // 已是毫秒。判定优先级：根元素 data-time-unit="s"（页面显式声明，
  // DOM 确定性——注入竞态下注册表可能尚未填充）→ __timelines 注册表
  // 存在（手写 HF 页兜底）→ 毫秒。采集端统一出毫秒，消费端不再各自换算。
  function hfScale() {
    try {
      var root = document.querySelector('[data-composition-id]');
      if (root && root.getAttribute('data-time-unit') === 's') {
        return 1000;
      }
      return (window.__timelines &&
              Object.keys(window.__timelines).length) ? 1000 : 1;
    } catch (e) { return 1; }
  }
  function report(tMs) {
    try {
      var els = document.querySelectorAll(SEL);
      var out = [];
      for (var i = 0; i < els.length; i++) {
        var el = els[i];
        var r = el.getBoundingClientRect();
        var o = {
          id: el.getAttribute('data-mc-id') || el.id || '',
          tag: (el.tagName || '').toLowerCase(),
          x: Math.round(r.left * 100) / 100,
          y: Math.round(r.top * 100) / 100,
          w: Math.round(r.width * 100) / 100,
          h: Math.round(r.height * 100) / 100
        };
        var TS = hfScale();
        var s = num(el.getAttribute('data-start'));
        if (s !== null) { o.start = Math.max(0, Math.round(s * TS)); }
        var d = num(el.getAttribute('data-duration'));
        if (d !== null) { o.duration = Math.max(0, Math.round(d * TS)); }
        var z = num(el.getAttribute('data-z'));
        if (z !== null) { o.z = Math.round(z); }
        // HyperFrames 原版词汇（data-layer = data-track-index 旧名）。
        var end = num(el.getAttribute('data-end'));
        if (end !== null) { o.end = Math.max(0, Math.round(end * TS)); }
        var track = num(el.getAttribute('data-track-index'));
        if (track === null) { track = num(el.getAttribute('data-layer')); }
        if (track !== null) { o.track_index = Math.round(track); }
        var ms = num(el.getAttribute('data-media-start'));
        if (ms !== null) { o.media_start = Math.max(0, Math.round(ms * TS)); }
        var ps = num(el.getAttribute('data-playback-start'));
        if (ps !== null) { o.playback_start = Math.max(0, Math.round(ps * TS)); }
        var pr = num(el.getAttribute('data-playback-rate'));
        if (pr !== null) { o.playback_rate = pr; }
        var lsrc = el.getAttribute('data-lottie-src');
        if (lsrc) { o.lottie_src = lsrc; }
        var csrc = el.getAttribute('data-composition-src');
        if (csrc) { o.composition_src = csrc; }
        var cvars = el.getAttribute('data-composition-variables');
        if (cvars) { o.composition_variables = cvars; }
        var cvals = el.getAttribute('data-variable-values');
        if (cvals) { o.variable_values = cvals; }
        var cid = el.getAttribute('data-composition-id');
        if (cid) { o.composition_id = cid; }
        // 音频/媒体链路（data-attributes.md Media 段）。
        var src = el.getAttribute('src') || el.getAttribute('data-src');
        if (src) { o.src = src; }
        var tag = (el.tagName || '').toLowerCase();
        if (tag === 'audio' || tag === 'video') {
          o.is_media = true;
        }
        var vol = num(el.getAttribute('data-volume'));
        if (vol !== null) { o.volume = vol; }
        var fin = num(el.getAttribute('data-fade-in'));
        if (fin !== null) { o.fade_in = Math.max(0, fin); }
        var fout = num(el.getAttribute('data-fade-out'));
        if (fout !== null) { o.fade_out = Math.max(0, fout); }
        var ha = el.getAttribute('data-has-audio');
        if (ha !== null) { o.has_audio = (ha === 'true'); }
        o.visible = (r.width > 0 && r.height > 0);
        out.push(o);
      }
      if (window.cefQuery) {
        window.cefQuery({
          request: 'mc.html.rects:' +
            JSON.stringify({t_ms: Math.max(0, Math.floor(Number(tMs) || 0)),
                            elements: out}),
          persistent: false,
          onSuccess: function () {},
          onFailure: function () {}
        });
      }
    } catch (e) {
      console.warn('[mc.html] rects collect failed', e);
    }
  }
  // Bake 时钟的应用侧：按 data-start/data-duration 切换元素可见性，使
  // 页面光栅只含当前生效元素——覆盖整页的背景元素若不隐藏后续元素，
  // 其纹理会把未生效元素一起带进合成（实测 bg 整页元素 → chip 在
  // start 前透出）。visibility 而非 display：保留布局，rect 元数据稳定。
  window.__mcApplyTimeline = function(tMs) {
    var t = Math.max(0, Number(tMs) || 0);
    var TS = hfScale();
    var els = document.querySelectorAll(SEL);
    for (var i = 0; i < els.length; i++) {
      var el = els[i];
      var s = num(el.getAttribute('data-start'));
      if (s !== null) { s *= TS; }
      var d = num(el.getAttribute('data-duration'));
      if (d !== null) { d *= TS; }
      var active = (s === null || t >= s) &&
                   (d === null || s === null || t < s + d);
      el.style.visibility = active ? 'visible' : 'hidden';
    }
    if (window.cefQuery) {
      window.cefQuery({
        request: 'mc.html.applied:' + t,
        persistent: false,
        onSuccess: function () {},
        onFailure: function () {}
      });
    }
  };
  window.__mcReportRects = report;
  report(0);
})();)js";
}

}  // namespace cef
}  // namespace xmedia
