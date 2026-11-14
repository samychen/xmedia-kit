#!/usr/bin/env python3
# xmedia-kit AI 视频生成管线：spec → 契约 HTML（brag 式 agent 面向层，
# 协议兼容 heygen-com/hyperframes 原版契约）。
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
# 产出：契约 HTML（兼容 HyperFrames 原版协议）——
#   data-composition-id 根 + clip data-start/duration(秒) + data-track-index
#   + paused GSAP 时间线注册 window.__timelines + 内联 GSAP（离线确定性）。
# 同 spec 两次生成逐字节一致（确定性）。

import json
import os
import re
import sys
import argparse
import hashlib

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
SUPPORTED_TYPES = ("title", "text", "outro",
                   "quote", "list", "stats", "comparison")

# 9:16 竖屏快捷预设（AI-auto-generate-video 对齐）。
FORMAT_PRESETS = {
    "16:9":  (1920, 1080),
    "9:16":  (1080, 1920),
    "1:1":   (1080, 1080),
    "4:5":   (1080, 1350),
}

# SFX 三级选择（AI-auto-generate-video 第 5 步对齐：场景覆盖 → 语义
# 关键词 → 类型默认）。素材 = assets/sfx/ 的 ffmpeg 合成音效（零授权）。
SFX_KEYWORDS = [
    ("突发|重磅|breaking|launch|发布", "whoosh"),
    ("成功|通过|达成|通关|冠军|success", "success"),
    ("完成|提示|叮|签到|ding", "ding"),
    ("点击|按钮|pop|切换", "pop"),
    ("转场|过渡|换幕|transition", "transition"),
]
SFX_TYPE_DEFAULT = {"title": "whoosh", "text": "pop", "outro": "ding",
                    "quote": None, "list": "pop", "stats": "success",
                    "comparison": "transition"}


def _pick_sfx(scene):
    """三级 SFX 选择：scene.sfx 覆盖 → 文本关键词匹配 → 类型默认。
    返回音效名（不含扩展名）或 None（不配）。"""
    custom = (scene.get("sfx") or "").strip()
    if custom:
        return custom
    haystack = " ".join(str(v) for v in
                        (scene.get("text"), scene.get("value"),
                         scene.get("description"), scene.get("quote")) if v)
    for pattern, sfx in SFX_KEYWORDS:
        if re.search(pattern, haystack, re.IGNORECASE):
            return sfx
    return SFX_TYPE_DEFAULT.get(scene.get("type", "text"))


def _resolve_sfx_dir(sfx_dir, spec_dir):
    """SFX 目录解析：用户自定义相对 spec 目录；内置 assets/sfx 相对仓库
    根（tools/ 的上级）。"""
    candidates = [os.path.join(spec_dir, sfx_dir),
                  os.path.join(os.path.dirname(os.path.dirname(
                      os.path.abspath(__file__))), sfx_dir)]
    seen = []
    for c in candidates:
        if c not in seen and os.path.isdir(c):
            seen.append(c)
    return seen


def _probe_audio_duration(path):
    """ffprobe 音频文件时长（秒）。路径相对于 spec 目录。"""
    import subprocess
    full = os.path.join(_spec_dir, path) if not os.path.isabs(path) else path
    try:
        result = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "csv=p=0", full], capture_output=True, text=True,
            timeout=10)
        if result.returncode == 0:
            return float(result.stdout.strip())
    except (ValueError, subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return None


def _fit_scene_duration(scene, fallback=3.0):
    """旁白驱动时长拟合：narration 音频时长 + padding（AI-auto-generate-
    video 对齐）。无 narration 时用 duration 字段或 fallback。"""
    narration = scene.get("narration")
    if narration:
        dur = _probe_audio_duration(narration)
        if dur is not None:
            padding = float(scene.get("padding", 0.5))
            return dur + padding
    return float(scene.get("duration", fallback))


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
    dur = _fit_scene_duration(scene, 3.0)
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
    elif typ == "quote":
        quote = esc(scene.get("quote", text))
        author = esc(scene.get("author", ""))
        inner = f'<blockquote class="scene-quote">&ldquo;{quote}&rdquo;</blockquote>'
        if author:
            inner += f'<p class="scene-quote-author">&mdash; {author}</p>'
    elif typ == "list":
        items = content if content else [{"text": t} for t in scene.get("items", [])]
        inner = f'<h2 class="scene-heading">{text}</h2>'
        inner += '<ul class="scene-list">'
        for c in items:
            cc = esc(c.get("text", "") if isinstance(c, dict) else str(c))
            inner += f'<li class="scene-list-item">{cc}</li>'
        inner += '</ul>'
    elif typ == "stats":
        stat_value = esc(scene.get("value", ""))
        stat_desc = esc(scene.get("description", ""))
        inner = f'<div class="scene-stat-value">{stat_value}</div>'
        if stat_desc:
            inner += f'<p class="scene-stat-desc">{stat_desc}</p>'
    elif typ == "comparison":
        left_col = esc(scene.get("left", ""))
        right_col = esc(scene.get("right", ""))
        left_items = scene.get("left_items", [])
        right_items = scene.get("right_items", [])
        inner = f'<h2 class="scene-heading">{text}</h2>'
        inner += '<div class="scene-comparison">'
        inner += f'<div class="comp-col"><h3>{left_col}</h3><ul>'
        for c in left_items:
            inner += f'<li>{esc(c)}</li>'
        inner += '</ul></div>'
        inner += f'<div class="comp-col"><h3>{right_col}</h3><ul>'
        for c in right_items:
            inner += f'<li>{esc(c)}</li>'
        inner += '</ul></div></div>'
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
    """GSAP 时间线：每场景入场动画（duration 进 toVars，4 参数正确形）。
    游标与 clip 窗口同源——同样走旁白拟合时长。"""
    lines = []
    cursor = 0.0
    for idx, scene in enumerate(scenes):
        dur = _fit_scene_duration(scene, 3.0)
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


def compose(spec, spec_sha=None, no_sfx=False, spec_name=None):
    """spec dict → 契约 HTML 字符串（兼容 HyperFrames 原版协议）。
    确定性：同输入同输出。spec_sha 为来源锚点（canonical JSON 的
    sha256），写进 HTML 头部生成器注释供产物验源。"""
    comp = spec.get("composition", {})
    comp_id = comp.get("id", "main")
    fmt = comp.get("format")
    if fmt and fmt in FORMAT_PRESETS:
        width, height = FORMAT_PRESETS[fmt]
    else:
        width = int(comp.get("width", 1920))
        height = int(comp.get("height", 1080))
    fps = int(comp.get("fps", 30))
    bg = esc(spec.get("background", "#000000"))
    scenes = spec.get("scenes", [])
    if not scenes:
        raise ValueError("spec.scenes 不能为空")

    # 生成场景 clip div
    scene_html_parts = []
    scene_starts = []
    start = 0.0
    for idx, scene in enumerate(scenes):
        scene_starts.append(start)
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

    # 逐场景旁白 → audio 元素（进 html_bake 确定性混音；时长 = 旁白实长，
    # 起点对齐场景 clip 窗口；BGM 默认 0.6 音量衬在旁白之下）。
    narr_audio = []
    for idx, scene in enumerate(scenes):
        narration = scene.get("narration")
        if not narration:
            continue
        ndur = _probe_audio_duration(narration)
        if ndur is None:
            print(f"hf_compose: 旁白无法探测时长，跳过混音轨 scene-{idx}: "
                  f"{narration}", file=sys.stderr)
            continue
        vol = float(scene.get("narration_volume", 1.0))
        narr_audio.append(f'  <audio src="{esc(narration)}" data-volume="{vol}"'
                          f' data-start="{scene_starts[idx]}"'
                          f' data-duration="{ndur}"></audio>\n')

    # SFX 音效（三级选择，随场景起点叠加进混音）：场景 sfx 覆盖 →
    # 文本关键词 → 类型默认；--no-sfx 或 sfx.enabled=false 关闭；素材
    # 目录解析 spec 目录优先、内置 assets/sfx 兜底，文件缺失跳过并提示。
    sfx_audio = []
    sfx_cfg = spec.get("sfx", {})
    if not no_sfx and sfx_cfg.get("enabled", True):
        sfx_dirs = _resolve_sfx_dir(str(sfx_cfg.get("dir", "assets/sfx")),
                                    _spec_dir)
        sfx_vol = float(sfx_cfg.get("volume", 0.4))
        missing = set()
        for idx, scene in enumerate(scenes):
            name = _pick_sfx(scene)
            if not name:
                continue
            hit = next((os.path.join(d, name + ".wav") for d in sfx_dirs
                        if os.path.isfile(os.path.join(d, name + ".wav"))),
                       None)
            if hit is None:
                missing.add(name)
                continue
            rel = os.path.relpath(hit, _spec_dir)
            if rel.startswith(".."):
                # 跨挂载点/跨目录的 relpath 会生成深 ".." 链——ffprobe 与
                # html_bake 的页面相对解析都不可靠，改绝对路径。
                rel = hit
            sdur = _probe_audio_duration(rel)
            if sdur is None:
                continue
            sfx_audio.append(
                f'  <audio src="{esc(rel)}" data-volume="{sfx_vol}"'
                f' data-start="{scene_starts[idx]}"'
                f' data-duration="{sdur}"></audio>\n')
        if missing:
            print(f"hf_compose: SFX 素材缺失（跳过）：{sorted(missing)} —— "
                  f"生成见 assets/sfx/README.md", file=sys.stderr)

    # 产物来源锚点：HTML 头部生成器注释（spec sha256 由 main 传入；
    # html_bake 再把 HTML sha256 写进 MP4 容器 metadata——MP4 → HTML →
    # spec 逐级验源）。
    provenance = f"<!-- xmedia-kit hf_compose | scenes={len(scenes)}"
    if spec_name:
        provenance += f" | spec:{spec_name}"
    if spec_sha:
        provenance += f" | spec sha256:{spec_sha}"
    provenance += " -->"

    # 场景样式（id 选择器：场景 div 的 class 是 scene-{type} 模板类，
    # 逐场景样式必须挂在 #scene-{idx} 上才命中）
    scene_styles = []
    for idx, scene in enumerate(scenes):
        bg = esc(scene.get("bg", "#000000"))
        color = esc(scene.get("text_color", "#ffffff"))
        scene_styles.append(
            f'  #scene-{idx} {{ background: {bg}; color: {color}; }}')

    style_block = "\n".join(scene_styles)

    html = f'''<!DOCTYPE html>
{provenance}
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
  .scene-quote {{ font-size: 48px; font-style: italic; line-height: 1.4;
                  max-width: 80%; }}
  .scene-quote-author {{ font-size: 24px; opacity: 0.7; margin-top: 20px; }}
  .scene-heading {{ font-size: 48px; font-weight: 600; margin-bottom: 24px; }}
  .scene-list {{ list-style: none; text-align: left; }}
  .scene-list-item {{ font-size: 30px; margin: 16px 0; padding-left: 24px;
                      border-left: 3px solid currentColor; }}
  .scene-stat-value {{ font-size: 120px; font-weight: 800; }}
  .scene-stat-desc {{ font-size: 28px; opacity: 0.8; margin-top: 12px; }}
  .scene-comparison {{ display: flex; gap: 48px; }}
  .comp-col {{ flex: 1; text-align: left; }}
  .comp-col h3 {{ font-size: 32px; font-weight: 600; margin-bottom: 16px;
                  border-bottom: 2px solid currentColor; padding-bottom: 8px; }}
  .comp-col li {{ font-size: 26px; margin: 12px 0; }}
{style_block}
</style>
</head>
<body>
  <div id="root" data-composition-id="{comp_id}" data-start="0"
       data-duration="{total_duration}" data-width="{width}"
       data-height="{height}" data-time-unit="s">
{chr(10).join(scene_html_parts)}
  </div>
{audio_tag}{''.join(narr_audio)}{''.join(sfx_audio)}  <script>{_gsap_src}</script>
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
    parser = argparse.ArgumentParser(description="xmedia-kit 契约 HTML 生成器")
    parser.add_argument("spec", help="spec JSON 文件路径")
    parser.add_argument("-o", "--output", default=None, help="输出 HTML 路径（默认 stdout）")
    parser.add_argument("--no-sfx", action="store_true",
                        help="关闭 SFX 音效（默认按场景类型/关键词自动配置）")
    args = parser.parse_args()

    try:
        with open(args.spec, "r", encoding="utf-8") as f:
            spec = json.load(f)
    except FileNotFoundError:
        print(f"hf_compose: spec 不存在：{os.path.abspath(args.spec)}\n"
              "  先写 spec JSON（schema 见 docs/HF_COMPOSE.md），或先跑 "
              "hf_tts.py 生成旁白版 spec", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"hf_compose: spec 不是合法 JSON：{e}", file=sys.stderr)
        sys.exit(1)

    global _spec_dir, _gsap_src
    _spec_dir = os.path.dirname(os.path.abspath(args.spec)) or "."
    _gsap_src = _load_gsap()
    # 来源锚点：canonical JSON（键排序、非 ASCII 原文）的 sha256——
    # 与文件格式化方式无关，同语义 spec 同哈希。
    spec_sha = hashlib.sha256(
        json.dumps(spec, sort_keys=True, ensure_ascii=False).encode(
            "utf-8")).hexdigest()
    html = compose(spec, spec_sha=spec_sha, no_sfx=args.no_sfx,
                   spec_name=os.path.basename(args.spec))
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(html)
        total = sum(_fit_scene_duration(s, 3.0) for s in spec.get("scenes", []))
        print(f"hf_compose: {args.output} — {len(spec.get('scenes', []))} scenes, "
              f"{total:.1f}s total")
    else:
        print(html)


if __name__ == "__main__":
    main()
