#!/usr/bin/env python3
# xmedia Studio：契约 HTML 的浏览器预览驱动（Player-lite）。
#
# 用法：hf_studio.py <contract.html> [-o studio.html]
#
# 在契约页注入传输条（播放/暂停/拖动/场景跳转）+ 驱动脚本——rAF 循环
# 驱动 __timelines（totalTime 秒制）与 __mcApplyTimeline（毫秒可见性），
# 与烘焙 seek 语义同源。单文件自包含，浏览器直接打开即用；契约页语义
# 不变（烘焙仍用原契约文件）。幂等：重复注入替换旧 chrome。
#
# 注意：注入的 chrome DOM 不携带 data-start/duration 属性——避免被
# __mcApplyTimeline 的可见性切换与 hf_validate 的 clip 统计误伤。

import argparse
import json
import os
import re
import sys

MARK_BEGIN = "<!-- xmedia-kit studio:begin -->"
MARK_END = "<!-- xmedia-kit studio:end -->"


def parse_scenes(html):
    """契约页的场景清单：div 且同时带 data-start/data-duration——合成根
    （data-composition-id）是容器不是场景，排除。
    返回 [{id, start, dur, label}]（秒）。"""
    scenes = []
    for m in re.finditer(r'<div\b([^>]*)>', html, re.S):
        attrs = m.group(1)
        if "data-composition-id" in attrs:
            continue
        start = re.search(r'data-start="([\d.]+)"', attrs)
        dur = re.search(r'data-duration="([\d.]+)"', attrs)
        sid_m = re.search(r'id="([^"]+)"', attrs)
        cls_m = re.search(r'class="([^"]*)"', attrs)
        if not (start and dur and sid_m):
            continue
        sid = sid_m.group(1)
        cls = cls_m.group(1) if cls_m else ""
        typ = next((c for c in cls.split() if c.startswith("scene-")
                    and c != "scene"), sid)
        scenes.append({"id": sid, "start": float(start.group(1)),
                       "dur": float(dur.group(1)), "label": typ})
    return scenes


def parse_total(html, scenes):
    """总时长（秒）：合成根 data-duration 优先，回退场景窗最大 end。"""
    m = re.search(r'data-composition-id[^>]*data-duration="([\d.]+)"',
                  html, re.S)
    if m:
        return float(m.group(1))
    return max((s["start"] + s["dur"] for s in scenes), default=0.0)


def build_chrome(total_s, scenes):
    """注入块：注释标记包裹的 chrome DOM + 驱动脚本。"""
    scenes_json = json.dumps(scenes, ensure_ascii=False)
    return f'''{MARK_BEGIN}
<style>
  #__mc-studio-chrome {{
    position: fixed; left: 0; right: 0; bottom: 0; height: 52px;
    background: rgba(12, 12, 20, 0.92); color: #e8e8f0;
    display: flex; align-items: center; gap: 10px; padding: 0 14px;
    font: 13px/1 -apple-system, "Helvetica Neue", sans-serif;
    z-index: 2147483647; user-select: none;
  }}
  #__mc-studio-chrome button {{
    background: #2a2a3c; color: #e8e8f0; border: 0; border-radius: 6px;
    padding: 6px 12px; cursor: pointer; font: inherit;
  }}
  #__mc-studio-chrome button:hover {{ background: #3a3a52; }}
  #__mc-studio-bar {{ flex: 1; accent-color: #6c8cff; }}
  #__mc-studio-scenes {{ display: flex; gap: 6px; max-width: 46%;
    overflow-x: auto; }}
  #__mc-studio-scenes button {{ padding: 5px 9px; font-size: 12px;
    white-space: nowrap; }}
  #__mc-studio-time {{ min-width: 96px; text-align: right;
    font-variant-numeric: tabular-nums; }}
</style>
<div id="__mc-studio-chrome">
  <button id="__mc-studio-play">▶</button>
  <input id="__mc-studio-bar" type="range" min="0" max="1000" value="0">
  <span id="__mc-studio-time">0.0 / {total_s:.1f}s</span>
  <div id="__mc-studio-scenes">
    <button data-goto="0">⏮</button>
{"".join(f'    <button data-goto="{s["start"]}">{s["label"]}</button>'
         for s in scenes)}
  </div>
</div>
<script>
(function() {{
  if (window.__mcStudio) {{ return; }}
  window.__mcStudio = true;
  var TOTAL = {total_s};
  var SCENES = {scenes_json};
  var playing = false, t = 0, raf = 0, last = 0;
  var bar = document.getElementById("__mc-studio-bar");
  var btn = document.getElementById("__mc-studio-play");
  var label = document.getElementById("__mc-studio-time");

  function drive(sec) {{
    // 与烘焙 seek 语义同源：注册表 totalTime（秒）+ 毫秒可见性。
    Object.values(window.__timelines || {{}}).forEach(function (tl) {{
      try {{
        if (typeof tl.pause === "function") {{ tl.pause(); }}
        if (typeof tl.totalTime === "function") {{
          tl.totalTime(sec + 1e-3, true);
        }} else if (typeof tl.seek === "function") {{ tl.seek(sec); }}
      }} catch (e) {{}}
    }});
    if (typeof window.__mcApplyTimeline === "function") {{
      try {{ window.__mcApplyTimeline(sec * 1000); }} catch (e) {{}}
    }} else if (typeof window.seek === "function") {{
      try {{ window.seek(sec * 1000); }} catch (e) {{}}
    }}
    bar.value = TOTAL > 0 ? Math.round(sec / TOTAL * 1000) : 0;
    label.textContent = sec.toFixed(1) + " / " + TOTAL.toFixed(1) + "s";
  }}
  function frame(now) {{
    if (!playing) {{ return; }}
    var dt = (now - last) / 1000; last = now;
    t = Math.min(TOTAL, t + dt);
    drive(t);
    if (t >= TOTAL) {{
      playing = false; btn.textContent = "▶";
      if (window.__mcStudioLoop) {{ t = 0; playing = true; last = now;
        raf = requestAnimationFrame(frame); }}
      return;
    }}
    raf = requestAnimationFrame(frame);
  }}
  function play() {{
    if (playing) {{ return; }}
    if (t >= TOTAL) {{ t = 0; }}
    playing = true; last = performance.now();
    btn.textContent = "⏸";
    raf = requestAnimationFrame(frame);
  }}
  function pause() {{
    playing = false; btn.textContent = "▶";
    if (raf) {{ cancelAnimationFrame(raf); raf = 0; }}
  }}
  btn.addEventListener("click", function () {{
    playing ? pause() : play();
  }});
  bar.addEventListener("input", function () {{
    pause();
    t = TOTAL * (parseFloat(bar.value) / 1000);
    drive(t);
  }});
  document.getElementById("__mc-studio-scenes")
    .addEventListener("click", function (ev) {{
      var g = ev.target.getAttribute && ev.target.getAttribute("data-goto");
      if (g === null || g === undefined) {{ return; }}
      pause(); t = parseFloat(g); drive(t);
    }});
  document.addEventListener("keydown", function (ev) {{
    if (ev.code === "Space" && ev.target === document.body) {{
      ev.preventDefault(); playing ? pause() : play();
    }} else if (ev.code === "ArrowLeft") {{
      pause(); t = Math.max(0, t - 1); drive(t);
    }} else if (ev.code === "ArrowRight") {{
      pause(); t = Math.min(TOTAL, t + 1); drive(t);
    }}
  }});
  drive(0);
}})();
</script>
{MARK_END}'''


def inject(html):
    """幂等注入：已存在 chrome 则先剥离旧块再重注入（spec 变更后可重生成）。"""
    if MARK_BEGIN in html:
        html = re.sub(re.escape(MARK_BEGIN) + r"[\s\S]*?" + re.escape(MARK_END),
                      "", html)
    scenes = parse_scenes(html)
    total = parse_total(html, scenes)
    chrome = build_chrome(total, scenes)
    if "</body>" in html:
        return html.replace("</body>", chrome + "\n</body>", 1)
    return html + "\n" + chrome


def main():
    parser = argparse.ArgumentParser(
        description="xmedia Studio：契约 HTML 浏览器预览驱动注入")
    parser.add_argument("html", help="契约 HTML（hf_compose 产出或过校验的手写页）")
    parser.add_argument("-o", "--output", default=None,
                        help="输出 studio HTML（默认 <stem>.studio.html）")
    args = parser.parse_args()

    src = os.path.abspath(args.html)
    with open(src, "r", encoding="utf-8") as f:
        html = f.read()
    already_studio = MARK_BEGIN in html
    if args.output:
        out = os.path.abspath(args.output)
    elif already_studio:
        out = src  # 对 studio 页重复运行 = 原地更新，不生成套娃文件名
    else:
        stem, ext = os.path.splitext(src)
        out = f"{stem}.studio{ext or '.html'}"

    scenes = parse_scenes(html)
    if not scenes:
        print("hf_studio: 页面无 data-start/data-duration 场景——"
              "不是契约 HTML？先跑 hf_compose.py 或 hf_validate.py",
              file=sys.stderr)
        sys.exit(1)

    with open(out, "w", encoding="utf-8") as f:
        f.write(inject(html))
    print(f"hf_studio: {out} — {len(scenes)} 场景 / "
          f"{parse_total(html, scenes):.1f}s，浏览器打开即预览")


if __name__ == "__main__":
    main()
