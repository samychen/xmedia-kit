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
SUPPORTED_SCHEMA = "1.0"  # spec schema_version；不匹配 → 拒绝并提示
SUPPORTED_TYPES = ("title", "text", "outro",
                   "quote", "list", "stats", "comparison")

# 自定义模板协议（对齐参考项目 slots 模式）：templates 目录（spec 目录
# 优先、内置 templates/ 兜底）下的 <name>.json = {name, description,
# slots, css, body, sfx}——body 的 {{slot}} 由场景字段替换。扫描在每次
# compose 时执行（新模板热加载）。
SLOT_RE = re.compile(r"\{\{(\w+)\}\}")


def load_custom_templates(spec_dir):
    """扫描模板目录 → {name: def}。spec_dir/templates 优先（用户覆盖），
    内置 templates/ 兜底；同名先到先得。"""
    registry = {}
    dirs = []
    builtin_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "templates")
    for d in (os.path.join(spec_dir or ".", "templates"), builtin_dir):
        if os.path.isdir(d) and d not in dirs:
            dirs.append(d)
    for d in dirs:
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".json"):
                continue
            try:
                with open(os.path.join(d, fn), "r", encoding="utf-8") as f:
                    defn = json.load(f)
                name = defn.get("name")
                if name and defn.get("body") and name not in registry:
                    defn["_dir"] = d
                    registry[name] = defn
            except (json.JSONDecodeError, OSError) as e:
                print(f"hf_compose: 模板 {fn} 解析失败跳过：{e}",
                      file=sys.stderr)
    return registry


def render_custom_template(defn, scene, idx):
    """自定义模板场景渲染：{{slot}} ← 场景同名字段（esc 转义，缺失
    置空）；返回 (inner_html, css 或 None)。"""
    body = defn.get("body", "")

    def sub(m):
        slot = m.group(1)
        val = scene.get(slot)
        if val is None:
            return ""
        if isinstance(val, (list, tuple)):
            # 列表值 = 无序列表条目（每项转义后包 <li>）——对比/清单类
            # 模板的 items slot 约定。
            return "".join(f"<li>{esc(str(x))}</li>" for x in val)
        return esc(str(val))

    body = SLOT_RE.sub(sub, body)
    name = defn.get("name", "custom")
    # id 与 class 同时发射：模板 CSS 常用 #tpl-<name> ID 选择器定位盒子
    inner = (f'  <div id="tpl-{name}" class="tpl-{name}" '
             f'data-tpl="{name}">\n{body}\n  </div>\n')
    css = defn.get("css") or None
    return inner, css

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


def _design_css(spec, bg_default):
    """spec.design 段（设计系统）→ (root CSS 变量, 场景类覆盖)。

    对齐 HF frame.md 设计系统层（frame-presets 的 YAML 规格）：colors
    带角色、typography ramp（display/h1/stat_figure/body/label）、
    components（hairline/invert_block）。场景显式 bg/color 仍优先。"""
    design = spec.get("design") or {}
    if not design:
        return "", ""
    colors = design.get("colors") or {}
    typo = design.get("typography") or {}
    comps = design.get("components") or {}
    root_lines = ["    --ground: " + colors.get("bg_primary", bg_default)]
    if colors.get("bg_secondary"):
        root_lines.append("    --surface: " + colors["bg_secondary"])
    root_lines.append("    --ink: " + colors.get("text_primary", "#ffffff"))
    if colors.get("text_secondary"):
        root_lines.append("    --ink-2: " + colors["text_secondary"])
    if colors.get("accent"):
        root_lines.append("    --accent-d: " + colors["accent"])
    if colors.get("line"):
        root_lines.append("    --line-d: " + colors["line"])

    def tvar(name, family, size, unit, weight, lh):
        if not family:
            return ""
        return ("    --t-" + name + ": " + family + " " + str(weight) + ";\n"
                + "    --t-" + name + "-size: calc(" + str(size) + " * 1"
                + unit + ");\n"
                + "    --t-" + name + "-lh: " + str(lh) + ";")

    scene_ov = []
    ink = colors.get("text_primary", "")
    ink2 = colors.get("text_secondary", "")
    disp = typo.get("display") or {}
    if disp.get("family"):
        scene_ov.append(
            "  .scene-title { font-family: " + disp["family"]
            + "; font-size: calc(" + str(disp.get("size_vh", 7)) + " * 1vh)"
            + "; font-weight: " + str(disp.get("weight", 700))
            + "; line-height: " + str(disp.get("line_height", 1.1))
            + "; letter-spacing: " + str(disp.get("tracking", "normal"))
            + ("; color: " + ink if ink else "")
            + "; }")
    if ink:
        scene_ov.append("  .scene > * { color: " + ink + "; }")
    body_t = typo.get("body") or {}
    if body_t.get("family"):
        scene_ov.append(
            "  .scene-body, .scene-item, .scene-list-item { font-family: "
            + body_t["family"] + "; font-size: calc("
            + str(body_t.get("size_vh", 1.0)) + " * 1vh); color: "
            + str(body_t.get("color") or ink2 or "inherit") + "; }")
    lbl = typo.get("label") or {}
    if lbl.get("family"):
        scene_ov.append(
            "  .scene-subtitle { font-family: " + lbl["family"]
            + "; font-size: " + str(lbl.get("px", 14)) + "px; letter-spacing: "
            + str(lbl.get("tracking", "normal")) + "; text-transform: "
            + ("uppercase" if lbl.get("upper") else "none") + "; color: "
            + str(lbl.get("color", "inherit")) + "; }")
    hl = comps.get("hairline") or ""
    if hl and "{colors.line}" in hl and colors.get("line"):
        scene_ov.append(
            "  .scene-heading { border-bottom: "
            + hl.replace("{colors.line}", colors["line"])
            + "; padding-bottom: 12px; }")
    root_css = "\n".join(root_lines)
    scene_css = "\n".join(scene_ov)
    return root_css, scene_css


def _design_presets():
    """内置设计预设（tools/design-presets/*.json）。"""
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "design-presets")
    out = {}
    if os.path.isdir(d):
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".json"):
                try:
                    data = json.load(open(os.path.join(d, fn),
                                          encoding="utf-8"))
                    out[data.get("name", fn[:-5])] = data
                except (json.JSONDecodeError, OSError):
                    pass
    return out


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


def tts_text(scene):
    """场景的可朗读/字幕文本：text 优先；缺失时按类型聚合专有字段
    （stats 取 value/description，quote 取 quote/author，list 取
    items，comparison 取列标题）——TTS 与字幕共用同一文本源。"""
    text = (scene.get("text") or "").strip()
    if text:
        return text
    typ = scene.get("type", "text")
    parts = []
    if typ == "stats":
        parts = [scene.get("value"), scene.get("description")]
    elif typ == "quote":
        parts = [scene.get("quote") or scene.get("text"), scene.get("author")]
    elif typ == "list":
        items = scene.get("items") or [c.get("text", "")
                                       for c in scene.get("content", [])]
        parts = [str(i) for i in items if str(i).strip()]
    elif typ == "comparison":
        parts = [scene.get("text"), scene.get("left"), scene.get("right")]
    parts = [str(p).strip() for p in parts if p and str(p).strip()]
    return "。".join(parts)


CAP_SEG_LIMIT = 400


def _norm_caption_segments(raw, duration):
    """规范化「场景相对」字幕段：排序、裁剪、去空、限量（确定性）。"""
    out = []
    for seg in (raw or [])[:CAP_SEG_LIMIT]:
        if not isinstance(seg, dict):
            continue
        try:
            a = max(0.0, float(seg.get("start", 0)))
            b = float(seg.get("end", 0))
        except (TypeError, ValueError):
            continue
        text = re.sub(r"\s+", " ", str(seg.get("text") or "")).strip()
        a, b = min(a, duration), min(b, duration)
        if b > a and text:
            out.append({"start": round(a, 3), "end": round(b, 3),
                        "text": text[:300]})
    out.sort(key=lambda s: (s["start"], s["end"], s["text"]))
    return out


def _clip_caption_segments(global_segs, scene_start, duration):
    """全局时间轴字幕 → 场景相对段，按场景窗口裁剪。"""
    clipped = []
    for seg in global_segs or []:
        a = max(float(seg["start"]), scene_start) - scene_start
        b = min(float(seg["end"]), scene_start + duration) - scene_start
        if b > a:
            clipped.append({"start": a, "end": b, "text": seg["text"]})
    return _norm_caption_segments(clipped, duration)


def render_caption_html(caption, segments, idx):
    """字幕条：有分段时每个段一个独立 div（初始 opacity:0，由 GSAP
    时间线按时间点亮，兼容 --clock seek 逐帧烘焙）；否则沿用整段字幕。"""
    if segments:
        parts = []
        for j, seg in enumerate(segments):
            parts.append(
                f'  <div id="cap-{idx}-{j}" class="scene-caption seg"'
                f' data-cap-start="{seg["start"]}"'
                f' data-cap-end="{seg["end"]}">'
                f'{esc(seg["text"])}</div>\n')
        return "".join(parts)
    if caption:
        return f'  <div class="scene-caption">{esc(caption)}</div>\n'
    return ""


def gen_scene_html(scene, start_sec, idx, caption=None, registry=None,
                   css_out=None, caption_segments=None):
    """单场景 → clip div HTML 片段。自定义模板类型（registry 命中）
    走 {{slot}} 渲染路径，其 css 经 css_out 回传进 style 块。"""
    typ = scene.get("type", "text")
    if registry and typ in registry:
        dur = _fit_scene_duration(scene, 3.0)
        inner, css = render_custom_template(registry[typ], scene, idx)
        if css and css_out is not None:
            css_out.append(css)
        caption_html = render_caption_html(
            caption, _norm_caption_segments(caption_segments, dur), idx)
        html = f'''  <div id="scene-{idx}" class="clip scene scene-custom"
       data-start="{start_sec}" data-duration="{dur}" data-track-index="{idx}"
       data-bg="transparent">
{inner}{caption_html}  </div>
'''
        return html, dur
    if typ not in SUPPORTED_TYPES:
        raise ValueError(f"scene[{idx}] 未知类型 '{typ}'（支持: {SUPPORTED_TYPES}"
                         f" + 自定义模板目录）")
    text = esc(scene.get("text", ""))
    subtitle = esc(scene.get("subtitle", ""))
    dur = _fit_scene_duration(scene, 3.0)
    bg = esc(scene.get("bg", "#000000"))
    color = esc(scene.get("text_color", "#ffffff"))
    content = scene.get("content", [])
    scene_id = f"scene-{idx}"

    # 场景类型 → 内部布局。设计语言（frame-worker 实测固化为模板默认）：
    # 左对齐偏置构图（.scene-stack 容器承担，去居中对称模板感）、一根
    # 发丝线分隔、编号等宽小标、outro 反转强调拍（ink 实块擦入反白）。
    # 两色纪律：强调只用反转/放大/密度，不引入第三色。
    HAIRLINE = '<i class="hairline"></i>'
    if typ == "title":
        inner = (f'<div class="scene-stack">{HAIRLINE}'
                 f'<h1 class="scene-title">{text}</h1>')
        if subtitle:
            inner += f'<p class="scene-subtitle">{subtitle}</p>'
        inner += '</div>'
    elif typ == "text":
        inner = f'<div class="scene-stack">{HAIRLINE}'
        inner += f'<p class="scene-body">{text}</p>'
        for c in content:
            cc = esc(c.get("text", ""))
            inner += f'<p class="scene-item">{cc}</p>'
        inner += '</div>'
    elif typ == "quote":
        quote = esc(scene.get("quote", text))
        author = esc(scene.get("author", ""))
        inner = (f'<div class="scene-stack">{HAIRLINE}'
                 f'<p class="scene-quote"><span class="qmark">&ldquo;</span>'
                 f'{quote}&rdquo;</p>')
        if author:
            inner += f'<p class="scene-quote-author">&mdash; {author}</p>'
        inner += '</div>'
    elif typ == "list":
        items = content if content else [{"text": t} for t in scene.get("items", [])]
        inner = (f'<div class="scene-stack">{HAIRLINE}'
                 f'<h2 class="scene-heading">{text}</h2>')
        inner += '<ul class="scene-list">'
        for j, c in enumerate(items):
            cc = esc(c.get("text", "") if isinstance(c, dict) else str(c))
            inner += (f'<li class="scene-list-item">'
                      f'<em class="num">{j + 1:02d}</em>{cc}</li>')
        inner += '</ul></div>'
    elif typ == "stats":
        stat_value = esc(scene.get("value", ""))
        stat_desc = esc(scene.get("description", ""))
        inner = (f'<div class="scene-stack">{HAIRLINE}'
                 f'<div class="scene-stat-value">{stat_value}</div>')
        if stat_desc:
            inner += f'<p class="scene-stat-desc">{stat_desc}</p>'
        inner += '</div>'
    elif typ == "comparison":
        left_col = esc(scene.get("left", ""))
        right_col = esc(scene.get("right", ""))
        left_items = scene.get("left_items", [])
        right_items = scene.get("right_items", [])
        inner = (f'<div class="scene-stack">{HAIRLINE}'
                 f'<h2 class="scene-heading">{text}</h2>')
        inner += '<div class="scene-comparison">'
        inner += f'<div class="comp-col"><h3>{left_col}</h3><ul>'
        for c in left_items:
            inner += f'<li>{esc(c)}</li>'
        inner += '</ul></div>'
        inner += f'<div class="comp-col"><h3>{right_col}</h3><ul>'
        for c in right_items:
            inner += f'<li>{esc(c)}</li>'
        inner += '</ul></div></div></div>'
    else:  # outro：反转强调拍——ink 实块擦入后文字反白（GSAP 编排）
        inner = (f'<i class="invert-block"></i>'
                 f'<h1 class="scene-title scene-inv">{text}</h1>')

    # 视频背景元素（场景级 video 字段）：铺满场景层，文本浮在其上；
    # data-start/duration 供烘焙侧逐帧对位（video.currentTime 同步）与
    # 音频混音（视频音轨按场景窗口叠加）。
    video = scene.get("video")
    video_tag = ""
    if video:
        vsrc = esc(video if isinstance(video, str) else
                   str(video.get("src", "")))
        # 源内起点（片段切割产物）：后段从源视频第 video_from 秒继续。
        # data-media-start 同时被视频原生层（画面 seek）与音频混音（音轨
        # 修剪）消费；单位与 data-start 一致（秒）——采集端按 hfScale 统一
        # 换算成毫秒，写毫秒会被再乘 1000。0 时不写，保证未切割的 spec
        # 产物逐字节不变。
        vfrom = float(scene.get("video_from") or 0)
        if isinstance(video, dict):
            vfrom = float(video.get("from") or vfrom)
        fattr = (f' data-media-start="{vfrom}"' if vfrom > 0 else "")
        video_tag = (f'  <video class="scene-video" src="{vsrc}"'
                     f' data-start="{start_sec}" data-duration="{dur}"{fattr}'
                     f' muted playsinline preload="auto"></video>\n')
    # 字幕轨（T3）：优先 ASR / 手工分段字幕（captions.segments 或场景
    # captions），否则回退到「旁白文本整段字幕」——TTS 与字幕同源，
    # 有 narration 才有整段字幕（captions.enabled=false 全局关闭）。
    caption_html = render_caption_html(
        caption, _norm_caption_segments(caption_segments, dur), idx)

    # 场景级 image（brand-kits logo 入包的模板层支持）：
    #   image: {src, mode}，mode = "backdrop"（铺满置底，文本浮上）|
    #   "corner"（角标水印，logo 常态）。src 走 esc() 转义；位置/大小
    #   由 CSS 侧 .scene-image-* 控制，不影响既有产物（无 image 字段
    #   时 zero-tag，产物逐字节不变）。
    image = scene.get("image") or {}
    image_tag = ""
    if isinstance(image, str):
        image = {"src": image}
    if image.get("src"):
        isrc = esc(str(image["src"]))
        imode = image.get("mode") or "backdrop"
        if imode not in ("backdrop", "corner"):
            imode = "backdrop"
        image_tag = (f'  <img class="scene-image scene-image-{imode}"'
                     f' src="{isrc}" alt="">\n')

    html = f'''  <div id="{scene_id}" class="clip scene scene-{typ}"
       data-start="{start_sec}" data-duration="{dur}" data-track-index="{idx}"
       data-bg="{bg}">
{image_tag}{video_tag}{inner}
{caption_html}  </div>
'''
    return html, dur


def _element_anim_js(idx, typ, base):
    """场景元素接力编排（frame-worker 的逐元素入场）：发丝线擦入 →
    主文起 → 次行 stagger。base = 元素编排起点（场景入场大半完成后）。
    全部 fromTo 挂同一条 paused 时间线（绝对秒）——seek 精确还原。
    outro 是反转强调拍：ink 实块擦入 → 文字反白。"""
    t = f"#scene-{idx}"
    L = []
    if typ == "outro":
        blk = f"{t} .invert-block"
        L.append(
            f'  tl.fromTo("{blk}", {{scaleX: 0}}, '
            f'{{scaleX: 1, duration: 0.55, ease: "power3.inOut"}}, '
            f'{round(base, 3)});')
        inv = f"{t} .scene-inv"
        L.append(
            f'  tl.fromTo("{inv}", {{opacity: 0}}, '
            f'{{opacity: 1, duration: 0.4, ease: "power2.out"}}, '
            f'{round(base + 0.42, 3)});')
        return L
    # 主文（title/body/heading/stat-value/quote）在发丝线后起
    prim = {"title": ".scene-title", "text": ".scene-body",
            "list": ".scene-heading", "stats": ".scene-stat-value",
            "quote": ".scene-quote", "comparison": ".scene-heading"}.get(typ)
    secs = {"title": [".scene-subtitle"], "text": [".scene-item"],
            "list": [".scene-list-item"], "stats": [".scene-stat-desc"],
            "quote": [".scene-quote-author"],
            "comparison": [".comp-col"]}.get(typ, [])
    if typ in ("title", "text", "quote", "list", "stats", "comparison"):
        L.append(
            f'  tl.fromTo("{t} .hairline", {{scaleX: 0}}, '
            f'{{scaleX: 1, duration: 0.45, ease: "power2.out"}}, '
            f'{round(base, 3)});')
    if prim:
        L.append(
            f'  tl.fromTo("{t} {prim}", {{y: 44, opacity: 0}}, '
            f'{{y: 0, opacity: 1, duration: 0.55, ease: "power2.out"}}, '
            f'{round(base + 0.22, 3)});')
    for j, sel in enumerate(secs):
        L.append(
            f'  tl.fromTo("{t} {sel}", {{y: 26, opacity: 0}}, '
            f'{{y: 0, opacity: 1, duration: 0.4, ease: "power2.out"}}, '
            f'{round(base + 0.5 + j * 0.14, 3)});')
    return L


def _render_template_timeline(defn, scene):
    """模板 timeline 片段的 slot 替换：{{slot}} → JS 字符串字面量
    （json 转义，与 body 的 HTML 转义不同通道——timeline 里是 JS 代码
    上下文）。列表值 join 成逗号分隔的字符串字面量序列。"""
    frag = defn.get("timeline") or ""
    if not frag:
        return ""

    def sub(m):
        slot = m.group(1)
        val = scene.get(slot)
        if val is None:
            return '""'
        if isinstance(val, (list, tuple)):
            return ", ".join(json.dumps(str(x), ensure_ascii=False)
                             for x in val)
        return json.dumps(str(val), ensure_ascii=False)

    return SLOT_RE.sub(sub, frag)


def gen_timeline_js(scenes, starts, durs, trans, trans_durs,
                    caption_segments=None, registry=None):
    """GSAP 时间线：每场景入场动画（duration 进 toVars，4 参数正确形）。
    游标与 clip 窗口同源（starts/durs 由 compose 的场景计划传入，含
    转场重叠）。转场类型 → 前一场景的出场动画（fade/slide/wipe）。
    caption_segments: {场景 idx: [场景相对段]}——逐段点亮/熄灭用零宽
    tween 挂在同一条 paused 时间线上，seek(t) 时 GSAP 会精确还原状态，
    因此 --clock seek 烘焙逐帧出图也能拿到正确字幕（不依赖 rAF/CSS 动画）。"""
    lines = []
    for idx, scene in enumerate(scenes):
        typ = scene.get("type", "text")
        # 场景级转场窗宽（compose 已按场景算好；标量也给，方便外部调用方）。
        trans_dur = (trans_durs[idx] if isinstance(trans_durs, (list, tuple))
                     else trans_durs)
        anim = scene.get("animation", DEFAULT_ANIMATION)
        # "none" = 不做入场动画（切割接缝：前半段的最后一帧与后半段的
        # 第一帧本是同一画面，再淡入一次就是凭空多出一个黑场）。
        # 其余未知值一律回落默认，保持旧 spec 的产物不变。
        if anim != "none" and anim not in ANIMATIONS:
            anim = DEFAULT_ANIMATION
        # 入场时长：转场场景钳到重叠窗宽（交叉期内完成过渡），其余 0.6s
        enter = 0.6
        has_trans = idx > 0 and trans[idx] != "none" and trans_dur > 0
        if has_trans:
            enter = max(0.3, min(0.6, trans_dur))
        # 转场优先于 animation:"none"：进入侧若瞬间满不透明，前一场景的
        # 出场动画全被盖住，等于又变回硬切。声明了转场就回落默认淡入
        # （wipe 走下面的 clip-path 分支，不在此列）。
        if anim == "none" and has_trans and trans[idx] != "wipe":
            anim = DEFAULT_ANIMATION
        target = f"#scene-{idx}"
        if anim == "none":
            pass  # 硬切进入：由 data-start 的可见性切换承担
        elif idx > 0 and trans[idx] == "wipe":
            # wipe = 进入侧 clip-path 揭示（不淡入）：新场景从左向右盖在
            # 旧场景上，旧场景保持原样直到窗尾——真正的擦除语义。
            lines.append(
                f'  tl.fromTo("{target}", '
                f'{{clipPath: "inset(0% 0% 0% 100%)"}}, '
                f'{{clipPath: "inset(0% 0% 0% 0%)", duration: {enter}, '
                f'ease: "power2.out"}}, {starts[idx]});')
            continue
        else:
            from_v, to_v = ANIMATIONS[anim]
            to_v = to_v[:-1] + f', duration: {enter}, ease: "power2.out"}}'
            lines.append(
                f'  tl.fromTo("{target}", {from_v}, {to_v}, {starts[idx]});')
        # 元素接力编排（发丝线/主文/stagger/反转拍）：入场大半完成后
        # 接力——转场交叉窗内元素动画继续进行，视觉上重叠自然。
        # anim=="none"（切割接缝）保持无编排。
        if anim != "none":
            lines.extend(_element_anim_js(idx, typ, starts[idx] + enter * 0.6))
        # 自定义模板编排：模板 JSON 的 timeline 字段（GSAP 语句片段，
        # {{slot}}=场景字段 JS 字面量、{tpl}=#tpl-<name>、{start}=场景
        # 绝对起点、{dur}=场景时长）——数字滚动/条形增长/逐字/乱码等
        # 模板级动效也走同一条 paused 主时间线，seek 确定性还原。
        # anim=="none"（切割接缝）同样跳过。
        if anim != "none" and registry and typ in registry:
            frag = _render_template_timeline(registry[typ], scene)
            if frag:
                frag = (frag.replace("{tpl}", f"#tpl-{typ}")
                            .replace("{start}", str(starts[idx]))
                            .replace("{dur}", str(round(durs[idx], 3))))
                lines.append(f"  /* tl[{typ}] */\n" + frag)
        # 转场交叉：该场景入场的同时，前一场景按类型出场。prev 取上一个
        # 非显式定位场景——多轨叠加层（scene.start 显式）不吃主轨交叉
        # 动画（它有自己的独立淡入），否则会被主轨场景的入场误淡出。
        if has_trans:
            prev_idx = idx - 1
            while prev_idx > 0 and scenes[prev_idx].get("start") is not None:
                prev_idx -= 1
            prev = f"#scene-{prev_idx}"
            ov = min(trans_dur, durs[prev_idx])
            if trans[idx] == "slide-left":
                lines.append(
                    f'  tl.to("{prev}", {{x: -120, opacity: 0, duration: {ov}, '
                    f'ease: "power1.in"}}, {starts[idx]});')
            elif trans[idx] == "slide-right":
                lines.append(
                    f'  tl.to("{prev}", {{x: 120, opacity: 0, duration: {ov}, '
                    f'ease: "power1.in"}}, {starts[idx]});')
            else:  # fade
                lines.append(
                    f'  tl.to("{prev}", {{opacity: 0, duration: {ov}, '
                    f'ease: "power1.in"}}, {starts[idx]});')
    # 分段字幕：每个 caption div 在自己的 [start, end) 窗口内 opacity=1。
    # 用 1ms 的 fromTo/to 而不是 CSS 动画——烘焙侧逐帧 seek 到任意 t，
    # GSAP 都会把这两条 tween 的状态正确还原。
    for idx in sorted((caption_segments or {}).keys()):
        base = starts[idx] if idx < len(starts) else 0.0
        for j, seg in enumerate(caption_segments[idx]):
            sel = f"#cap-{idx}-{j}"
            a, b = round(base + seg["start"], 3), round(base + seg["end"], 3)
            lines.append(f'  tl.set("{sel}", {{opacity: 0}}, 0);')
            lines.append(f'  tl.set("{sel}", {{opacity: 1}}, {a});')
            lines.append(f'  tl.set("{sel}", {{opacity: 0}}, {b});')
    return "\n".join(lines)


def _load_gsap():
    """内联 GSAP（离线确定性：不依赖 CDN/file:// 子资源）。

    查找顺序有讲究：spec 目录优先（用户可钉版本），然后是 tools/ 自带的一
    份，最后才回落到 tests/testdata/（源码树的路径）。tools/ 那份是为
    npm 包准备的——`npx xmedia-kit` 装出来没有 tests/ 目录，以前会静默
    生成 `/* gsap not found */`，产物能动但所有动画都不跑。

    gsap-plugins.min.js（SplitText/ScrambleText 等，GSAP 3.13 起官方
    免费开源）存在则追加——文字逐字/乱码揭示动效（HF glyph-scramble
    同款）的依赖；缺失时静默降级（模板 timeline 里的引用由页面
    typeof 守卫兜底）。
    """
    tools_dir = os.path.dirname(os.path.abspath(__file__))

    def _find(name):
        for candidate in [
            os.path.join(_spec_dir, name),
            os.path.join(tools_dir, name),
            os.path.join(tools_dir, "..", "tests", "testdata", name),
        ]:
            if os.path.isfile(candidate):
                return candidate
        return None

    core = _find("gsap.min.js")
    if not core:
        return "/* gsap not found */"
    src = ""
    with open(core, "r", encoding="utf-8") as f:
        src += f.read()
    plugins = _find("gsap-plugins.min.js")
    if plugins:
        with open(plugins, "r", encoding="utf-8") as f:
            src += "\n" + f.read()
    return src


def compose(spec, spec_sha=None, no_sfx=False, spec_name=None):
    ver = str(spec.get("schema_version", SUPPORTED_SCHEMA))
    if ver != SUPPORTED_SCHEMA:
        raise ValueError(
            f"spec schema_version {ver} 不受支持（本构建支持 "
            f"{SUPPORTED_SCHEMA}）——升级 xmedia-kit 或修正 schema_version")
    """spec dict → 契约 HTML 字符串（兼容 HyperFrames 原版协议）。
    确定性：同输入同输出。spec_sha 为来源锚点（canonical JSON 的
    sha256），写进 HTML 头部生成器注释供产物验源。"""
    # 直接调用 compose()（测试、Studio 之外的嵌入方）时 CLI 的 main 没跑过，
    # _gsap_src 还是空串——页面里就只剩 `const tl = gsap.timeline()`，一执行
    # 就抛 ReferenceError，所有入场/转场静默消失。首次使用时补加载。
    global _gsap_src
    if not _gsap_src:
        _gsap_src = _load_gsap()
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

    # 场景计划（含转场重叠）：全局 transitions 段为默认，场景级
    # transition 字段覆盖；重叠窗内下一场景提前进入、上一场景按类型
    # 出场（GSAP 交叉动画），总时长随重叠收缩。
    trans_cfg = spec.get("transitions", {})
    trans_dur = float(trans_cfg.get("duration", 0.5))
    # 字幕轨（T3）：captions.segments 是「全局时间轴绝对秒」的 ASR 字幕段
    # （hf_asr.py 的 captions.json 可直接填进来）；场景级 captions 是场景
    # 相对秒。两者都没有时才回退到「旁白文本整段字幕」。
    cap_cfg = spec.get("captions") or {}
    captions_on = cap_cfg.get("enabled", True)
    global_caps = _norm_caption_segments(cap_cfg.get("segments") or [],
                                         86400.0)
    cap_by_scene = {}
    trans_default = str(trans_cfg.get("type", "fade") or "none")
    scene_html_parts = []
    scene_starts = []
    starts = []
    durs = []
    trans = []
    trans_durs = []
    cursor = 0.0
    registry = load_custom_templates(_spec_dir)
    tpl_css = []
    for idx, scene in enumerate(scenes):
        ttype = (str(scene.get("transition") or trans_default) or "none")
        if ttype not in ("fade", "slide-left", "slide-right", "wipe", "none"):
            ttype = "none"
        # 场景级转场的窗口：全局 transitions.duration 是默认值，但它为 0
        # 时场景级 transition 会连窗口都没有——类型有了、重叠窗是 0，
        # 面板上选了 slide / wipe 却"没反应"就是这条。显式声明转场的场景
        # 回落到 0.5s；只吃全局默认的场景仍按 0 处理（旧 spec 行为不变）。
        sdur = trans_dur
        if sdur <= 0 and scene.get("transition") and ttype != "none":
            sdur = 0.5
        # 多轨显式定位（HF 多视频轨语义）：scene.start 存在 = 该场景绝对
        # 定位（叠加层，可与主轨重叠），不占主轨游标、转场交叉跳过。
        explicit = scene.get("start")
        if explicit is not None:
            pos_start = float(explicit)
            trans_durs.append(0.0)
        else:
            if idx > 0:
                # 转场归属进入侧：本场景提前 sdur 进入前一场景的尾窗
                cursor -= (sdur if ttype != "none" else 0.0)
            pos_start = cursor
            trans_durs.append(sdur)
        scene_starts.append(pos_start)
        starts.append(pos_start)
        caption = None
        cap_segs = None
        if captions_on:
            local = scene.get("captions")
            hint = _fit_scene_duration(scene, 3.0)
            if isinstance(local, list) and local:
                cap_segs = _norm_caption_segments(local, hint)
            elif global_caps:
                cap_segs = _clip_caption_segments(global_caps, pos_start,
                                                  hint)
            if not cap_segs and scene.get("narration"):
                cap_text = tts_text(scene)
                if cap_text:
                    caption = cap_text
        html, dur = gen_scene_html(scene, pos_start, idx, caption=caption,
                                   registry=registry, css_out=tpl_css,
                                   caption_segments=cap_segs)
        if cap_segs:
            cap_by_scene[idx] = cap_segs
        scene_html_parts.append(html)
        durs.append(dur)
        trans.append(ttype)
        if explicit is None:
            cursor = pos_start + dur
    total_duration = max([cursor] + [starts[i] + durs[i]
                                     for i in range(len(scenes))])

    # GSAP 时间线（含分段字幕的点亮/熄灭 tween）
    timeline_js = gen_timeline_js(scenes, starts, durs, trans, trans_durs,
                                  cap_by_scene, registry=registry)

    # 手绘标注层（Draw 画笔 → 成片）：spec.annotations = [{points:
    # [[x,y]…]（0-1 归一化）, color, width}]——契约页 SVG 叠加
    # （viewBox 1000、preserveAspectRatio none 与画面比例无关），
    # 全片窗口（data-start=0 / duration=total）——预览与导出同源。
    ann = spec.get("annotations") or []
    ann_svg = ""
    polys = []
    for a in ann:
        pts = a.get("points") or []
        if len(pts) < 2:
            continue
        pts_s = " ".join(
            "{:.0f},{:.0f}".format(
                max(0, min(1, float(p[0]))) * 1000,
                max(0, min(1, float(p[1]))) * 1000)
            for p in pts if isinstance(p, (list, tuple)) and len(p) >= 2)
        if not pts_s:
            continue
        polys.append(
            '<polyline points="' + pts_s + '" fill="none" stroke="' +
            esc(a.get("color", "#ff4d9d")) + '" stroke-width="' +
            esc(str(a.get("width", 4))) + '" stroke-linecap="round"'
            ' stroke-linejoin="round"/>')
    if polys:
        ann_svg = ('  <svg class="ann-layer" data-start="0" data-duration="' +
                   str(total_duration) + '" viewBox="0 0 1000 1000"'
                   ' preserveAspectRatio="none" style="position:absolute;'
                   ' inset:0; width:100%; height:100%; z-index:5;'
                   ' pointer-events:none">\n    ' + "".join(polys) +
                   "\n  </svg>\n")

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
    provenance = (f"<!-- xmedia-kit hf_compose | scenes={len(scenes)}"
                  f" | schema:{ver}")
    if spec_name:
        provenance += f" | spec:{spec_name}"
    if spec_sha:
        provenance += f" | spec sha256:{spec_sha}"
    provenance += " -->"

    # 场景样式（id 选择器：场景 div 的 class 是 scene-{type} 模板类，
    # 逐场景样式必须挂在 #scene-{idx} 上才命中）。视频场景：背景透明
    # （画面由烘焙侧原生视频层供给，光栅只承载文字），video 元素本身
    # visibility:hidden 不进光栅——CEF OSR 的 <video> 合成不随 seek
    # 重绘（实测冻结），像素走 FFmpeg 原生解码。
    has_video = any(scene.get("video") for scene in scenes)
    body_bg = "transparent" if has_video else "var(--ground)"
    scene_styles = []
    for idx, scene in enumerate(scenes):
        bg = esc(scene.get("bg", "#000000"))
        color = esc(scene.get("text_color", "#ffffff"))
        bg_style = "transparent" if scene.get("video") else bg
        scene_styles.append(
            f'  #scene-{idx} {{ background: {bg_style}; color: {color}; }}')

    design_root, design_scene = _design_css(spec, bg)
    style_block = "\n".join(scene_styles)
    if design_scene:
        design_scene = "\n" + design_scene
    if tpl_css:
        style_block += "\n" + "\n".join(tpl_css)

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
{design_root}
  }}
  * {{ margin: 0; padding: 0; box-sizing: border-box; }}
  html, body {{
    width: {width}px; height: {height}px;
    overflow: hidden; background: {body_bg};
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
  .scene-video {{
    position: absolute; inset: 0; width: 100%; height: 100%;
    object-fit: cover; visibility: hidden;
  }}
  .scene-image {{
    position: absolute; inset: 0; width: 100%; height: 100%;
    object-fit: cover; z-index: 0;
  }}
  .scene-image-corner {{
    inset: auto; width: auto; height: auto; object-fit: contain;
    right: 4%; bottom: 5%; max-width: 18%; max-height: 12%;
    opacity: .92;
  }}
  .scene > :not(.scene-video) {{
    position: relative; z-index: 1;
  }}
  .scene > .scene-caption {{
    position: absolute; left: 50%; transform: translateX(-50%);
    bottom: 6%; max-width: 84%; z-index: 2;
    background: rgba(0, 0, 0, 0.45); color: #ffffff;
    font-size: 30px; line-height: 1.4; padding: 10px 22px;
    border-radius: 10px; text-align: center;
  }}
  .scene > .scene-caption.seg {{ opacity: 0; }}
  /* ---- 模板设计语言（frame-worker 实测固化为默认，design 段可覆盖）----
     左对齐偏置构图：文字块不居中——上 20vh / 左 8vw 起，视觉重心偏置；
     发丝线：一根 2px ink 线做呼吸分隔；两色纪律：强调只用反转/放大/
     密度。字号按竖屏 1920 高的 vh 基准取胆量（title 8vh≈154px）。 */
  .scene-stack {{
    width: 84%; max-width: 84%;
    display: flex; flex-direction: column; align-items: flex-start;
    text-align: left;
  }}
  .hairline {{
    display: block; width: 64px; height: 2px;
    background: currentColor; opacity: .85;
    transform-origin: left center; margin-bottom: 3.2vh;
  }}
  .scene-title {{
    font-size: 8vh; font-weight: 800; line-height: 1.14;
    letter-spacing: -0.02em; max-width: 100%;
    text-wrap: balance;
  }}
  .scene-subtitle {{ font-size: 2.3vh; opacity: 0.72; margin-top: 2.4vh;
                     font-weight: 400; }}
  .scene-body {{ font-size: 3.4vh; line-height: 1.55; max-width: 100%; }}
  .scene-item {{ font-size: 2.4vh; opacity: 0.82; margin-top: 1.6vh;
                 font-weight: 400; }}
  .scene-quote {{ font-size: 4.4vh; line-height: 1.42; font-weight: 700;
                  max-width: 100%; }}
  .scene-quote .qmark {{ color: currentColor; opacity: .5; margin-right: .4vh;
                         font-weight: 800; }}
  .scene-quote-author {{ font-size: 1.9vh; opacity: 0.62; margin-top: 2.6vh;
                         font-weight: 400; }}
  .scene-heading {{ font-size: 4.8vh; font-weight: 800; line-height: 1.2;
                    margin-bottom: 3.4vh; letter-spacing: -0.01em;
                    max-width: 100%; }}
  .scene-list {{ list-style: none; text-align: left; }}
  .scene-list-item {{ font-size: 2.9vh; line-height: 1.4; margin: 1.9vh 0;
                      display: flex; align-items: baseline; }}
  .scene-list-item .num {{
    font-family: ui-monospace, Menlo, monospace;
    font-size: 1.7vh; opacity: .5; margin-right: 1.6vh;
    font-style: normal; flex: none;
  }}
  .scene-stat-value {{
    font-size: 15vh; font-weight: 800; letter-spacing: -0.03em;
    line-height: 1.05;
  }}
  .scene-stat-desc {{ font-size: 2.4vh; opacity: 0.78; margin-top: 2vh;
                      font-weight: 400; }}
  .scene-comparison {{ display: flex; gap: 6vw; width: 100%; }}
  .comp-col {{ flex: 1; text-align: left; }}
  .comp-col h3 {{ font-size: 2.9vh; font-weight: 800; margin-bottom: 1.8vh;
                  border-bottom: 2px solid currentColor; padding-bottom: 1.2vh; }}
  .comp-col li {{ font-size: 2.2vh; margin: 1.4vh 0; opacity: .9; }}
  /* outro 反转强调拍：ink 实块盖满后文字反白（GSAP 编排驱动）。 */
  .invert-block {{
    position: absolute; inset: 0; background: var(--ink);
    transform: scaleX(0); transform-origin: left center;
    z-index: 2;
  }}
  .scene-inv {{ color: var(--ground); opacity: 0; position: relative;
                z-index: 3; width: 84%; text-align: left; }}
{design_scene}
{style_block}
</style>
</head>
<body>
  <div id="root" data-composition-id="{comp_id}" data-start="0"
       data-duration="{total_duration}" data-width="{width}"
       data-height="{height}" data-time-unit="s" data-fps="{fps}">
{chr(10).join(scene_html_parts)}
  </div>
{ann_svg}{audio_tag}{''.join(narr_audio)}{''.join(sfx_audio)}  <script>{_gsap_src}</script>
  <script>
    window.__timelines = window.__timelines || {{}};
    if (window.SplitText) {{ gsap.registerPlugin(SplitText); }}
    if (window.ScrambleTextPlugin) {{ gsap.registerPlugin(ScrambleTextPlugin); }}
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
    try:
        html = compose(spec, spec_sha=spec_sha, no_sfx=args.no_sfx,
                       spec_name=os.path.basename(args.spec))
    except ValueError as e:
        print(f"hf_compose: {e}", file=sys.stderr)
        sys.exit(1)
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
