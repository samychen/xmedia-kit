// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// MakeRectsScript 的定义：独立文件，避免往 cef_osr_client.cpp（861 行）
// 里做增量手术。声明在 cef_osr_client.h（namespace xmedia::cef）。

#include "media/cef/cef_osr_client.h"

namespace xmedia {
namespace cef {

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
        var s = num(el.getAttribute('data-start'));
        if (s !== null) { o.start = Math.max(0, Math.round(s)); }
        var d = num(el.getAttribute('data-duration'));
        if (d !== null) { o.duration = Math.max(0, Math.round(d)); }
        var z = num(el.getAttribute('data-z'));
        if (z !== null) { o.z = Math.round(z); }
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
  window.__mcReportRects = report;
  report(0);
})();)js";
}

}  // namespace cef
}  // namespace xmedia
