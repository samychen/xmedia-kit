#!/usr/bin/env python3
# HyperFrames 契约 HTML 生成器（brag 式 AI 能力的 agent 面向层）。
#
# 用法：hf_compose.py <spec.json> [-o output.html]
#
# spec JSON schema（agent 按 schema 填空，生成器保证契约合规）：
# {
#   "composition": {"id": "main", "width": 1920, "height": 1080, "fps": 30},
#   "background": "#000000",
#   "scenes": [
#     {"type": "title", "text": "标题", "subtitle": "副标题",
#      "duration": 3, "bg": "#1a1a2e", "text_color": "#ffffff",
#      "animation": "fade_in_up"},
#     {"type": "text", "text": "正文内容",
#      "duration": 4, "bg": "#16213e", "text_color": "#e0e0e0",
#      "animation": "slide_in_left"},
#     {"type": "outro", "text": "结束画面",
#      "duration": 2, "bg": "#0f3460", "text_color": "#ffffff",
#      "animation": "fade_in"}
#   ],
#   "audio": {"src": "bgm.mp3", "volume": 0.6, "fade_in": 1, "fade_out": 2}
# }
#
# 产出：HyperFrames 原版契约 HTML——
#   data-composition-id 根 + clip data-start/duration(秒) + data-track-index
#   + paused GSAP 时间线注册 window.__timelines + 内联 GSAP（离线确定性）。
# 同 spec 两次生成逐字节一致（确定性）。

import json
import os
import sys
import argparse

_spec_dir = '.'
_gsap_src = ''

# 入场动画预设 → GSAP fromTo 片段（duration 秒由调用方传入）。
ANIMATIONS = {
    "fade_in":      ("{opacity: 0}", "{opacity: 1}"),
    "fade_in_up":   ("{opacity: 0, y: 60}", "{opacity: 1, y: 0}"),
    "fade_in_down": ("{opacity: 0, y: -60}", "{opacity: 1, y: 0}"),
    "slide_in_left":  ("{opacity: 0, x: -200}", "{opacity: 1, x: 0}"),
    "slide_in_right": ("{opacity: 0, x: 200}", "{opacity: 1, x: 0}"),
    "scale_in":     ("{opacity: 0, scale: 0.8}", "{opacity: 1, scale: 1}"),
}

DEFAULT_ANIMATION = "fade_in"
SUPPORTED_TYPES = ("title", "text", "outro")


def esc(s):
    """HTML 转义（防注入——spec 是 agent 生成的，不完全信任）。"""
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def gen_scene_html(scene, start_sec, idx):
    """单场景 → clip div HTML 片段。"""
    typ = scene.get("type", "text")
    if typ not in SUPPORTED_TYPES:
        raise ValueError(f"scene[{idx}] 未知类型 '{typ}'（支持: {SUPPORTED_TYPES}）")
    text = esc(scene.get("text", ""))
    subtitle = esc(scene.get("subtitle", ""))
    dur = float(scene.get("duration", 3))
    bg = esc(scene.get("bg", "#000000"))
    color = esc(scene.get("text_color", "#ffffff"))
    content = scene.get("content", [])
    scene_id = f"scene-{idx}"

    # 场景类型 → 内部布局
    if typ == "title":
        inner = f'<h1 class="scene-title">{text}</h1>'
        if subtitle:
            inner += f'<p class="scene-subtitle">{subtitle}</p>'
    elif typ == "text":
        inner = f'<p class="scene-body">{text}</p>'
        for c in content:
            cc = esc(c.get("text", ""))
            inner += f'<p class="scene-item">{cc}</p>'
    else:  # outro
        inner = f'<h1 class="scene-title">{text}</h1>'

    html = f'''  <div id="{scene_id}" class="clip scene scene-{typ}"
       data-start="{start_sec}" data-duration="{dur}" data-track-index="{idx}"
       data-bg="{bg}">
{inner}
  </div>
'''
    return html, dur


def gen_timeline_js(scenes):
    """GSAP 时间线：每场景入场动画（duration 进 toVars，4 参数正确形）。"""
    lines = []
    cursor = 0.0
    for idx, scene in enumerate(scenes):
        dur = float(scene.get("duration", 3))
        anim = scene.get("animation", DEFAULT_ANIMATION)
        if anim not in ANIMATIONS:
            anim = DEFAULT_ANIMATION
        from_v, to_v = ANIMATIONS[anim]
        # duration/ease 进 toVars（GSAP fromTo 的第 3 参数）
        to_v = to_v[:-1] + ', duration: 0.6, ease: "power2.out"}'
        target = f"#scene-{idx}"
        lines.append(f'  tl.fromTo("{target}", {from_v}, {to_v}, {cursor});')
        cursor += dur
    return "\n".join(lines)


def _load_gsap():
    """内联 GSAP（离线确定性：不依赖 CDN/file:// 子资源）。"""
    for candidate in [
        os.path.join(_spec_dir, "gsap.min.js"),
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                     "tests", "testdata", "gsap.min.js"),
    ]:
        if os.path.isfile(candidate):
            with open(candidate, "r", encoding="utf-8") as f:
                return f.read()
    return "/* gsap not found */"


def compose(spec):
    """spec dict → HyperFrames 契约 HTML 字符串。确定性：同输入同输出。"""
    comp = spec.get("composition", {})
    comp_id = comp.get("id", "main")
    width = int(comp.get("width", 1920))
    height = int(comp.get("height", 1080))
    fps = int(comp.get("fps", 30))
    bg = esc(spec.get("background", "#000000"))
    scenes = spec.get("scenes", [])
    if not scenes:
        raise ValueError("spec.scenes 不能为空")

    # 生成场景 clip div
    scene_html_parts = []
    start = 0.0
    for idx, scene in enumerate(scenes):
        html, dur = gen_scene_html(scene, start, idx)
        scene_html_parts.append(html)
        start += dur
    total_duration = start

    # GSAP 时间线
    timeline_js = gen_timeline_js(scenes)

    # 音频
    audio = spec.get("audio", {})
    audio_tag = ""
    if audio.get("src"):
        src = esc(audio["src"])
        vol = float(audio.get("volume", 0.6))
        audio_tag = f'  <audio src="{src}" data-volume="{vol}" data-start="0" data-duration="{total_duration}"></audio>\n'

    # 场景样式
    scene_styles = []
    for idx, scene in enumerate(scenes):
        bg = esc(scene.get("bg", "#000000"))
        color = esc(scene.get("text_color", "#ffffff"))
        scene_styles.append(
            f'  .scene-{idx} {{ background: {bg}; color: {color}; }}')

    style_block = "\n".join(scene_styles)

    html = f'''<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width={width}, height={height}" />
<style>
  :root {{
    --ground: {bg};
    --ink: #ffffff;
    --font-display: sans-serif;
  }}
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  html, body {{
    width: {width}px; height: {height}px;
    overflow: hidden; background: var(--ground);
  }}
  #root {{
    width: 100%; height: 100%;
    display: flex; flex-direction: column;
    align-items: center; justify-content: center;
    font-family: var(--font-display);
  }}
  .scene {{
    position: absolute; inset: 0;
    display: flex; flex-direction: column;
    align-items: center; justify-content: center;
  }}
  .scene-title {{ font-size: 72px; font-weight: 700; }}
  .scene-subtitle {{ font-size: 36px; opacity: 0.8; margin-top: 16px; }}
  .scene-body {{ font-size: 42px; line-height: 1.6; }}
  .scene-item {{ font-size: 28px; opacity: 0.85; margin-top: 12px; }}
{style_block}
</style>
</head>
<body>
  <div id="root" data-composition-id="{comp_id}" data-start="0"
       data-duration="{total_duration}" data-width="{width}"
       data-height="{height}">
{chr(10).join(scene_html_parts)}
  </div>
{audio_tag}  <script>{_gsap_src}</script>
  <script>
    window.__timelines = window.__timelines || {{}};
    const tl = gsap.timeline({{ paused: true }});
{timeline_js}
    window.__timelines["{comp_id}"] = tl;
    tl.seek(0);
  </script>
</body>
</html>
'''
    return html


def main():
    parser = argparse.ArgumentParser(description="HyperFrames 契约 HTML 生成器")
    parser.add_argument("spec", help="spec JSON 文件路径")
    parser.add_argument("-o", "--output", default=None, help="输出 HTML 路径（默认 stdout）")
    args = parser.parse_args()

    with open(args.spec, "r", encoding="utf-8") as f:
        spec = json.load(f)

    global _spec_dir, _gsap_src
    _spec_dir = os.path.dirname(os.path.abspath(args.spec)) or "."
    _gsap_src = _load_gsap()
    html = compose(spec)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(html)
        total = sum(float(s.get("duration", 3)) for s in spec.get("scenes", []))
        print(f"hf_compose: {args.output} — {len(spec.get('scenes', []))} scenes, "
              f"{total:.1f}s total")
    else:
        print(html)


if __name__ == "__main__":
    main()
