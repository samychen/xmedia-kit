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
        return esc(str(val)) if val is not None else ""

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


def gen_timeline_js(scenes, starts, durs, trans, trans_durs,
                    caption_segments=None):
    """GSAP 时间线：每场景入场动画（duration 进 toVars，4 参数正确形）。
    游标与 clip 窗口同源（starts/durs 由 compose 的场景计划传入，含
    转场重叠）。转场类型 → 前一场景的出场动画（fade/slide/wipe）。
    caption_segments: {场景 idx: [场景相对段]}——逐段点亮/熄灭用零宽
    tween 挂在同一条 paused 时间线上，seek(t) 时 GSAP 会精确还原状态，
    因此 --clock seek 烘焙逐帧出图也能拿到正确字幕（不依赖 rAF/CSS 动画）。"""
    lines = []
    for idx, scene in enumerate(scenes):
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
        # 转场交叉：该场景入场的同时，前一场景按类型出场
        if has_trans:
            prev = f"#scene-{idx-1}"
            ov = min(trans_dur, durs[idx - 1])
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
    """
    tools_dir = os.path.dirname(os.path.abspath(__file__))
    for candidate in [
        os.path.join(_spec_dir, "gsap.min.js"),
        os.path.join(tools_dir, "gsap.min.js"),
        os.path.join(tools_dir, "..", "tests", "testdata", "gsap.min.js"),
    ]:
        if os.path.isfile(candidate):
            with open(candidate, "r", encoding="utf-8") as f:
                return f.read()
    return "/* gsap not found */"


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
    start = 0.0
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
        if idx > 0:
            # 转场归属进入侧：本场景提前 sdur 进入前一场景的尾窗
            start -= (sdur if ttype != "none" else 0.0)
        trans_durs.append(sdur)
        scene_starts.append(start)
        starts.append(start)
        caption = None
        cap_segs = None
        if captions_on:
            local = scene.get("captions")
            hint = _fit_scene_duration(scene, 3.0)
            if isinstance(local, list) and local:
                cap_segs = _norm_caption_segments(local, hint)
            elif global_caps:
                cap_segs = _clip_caption_segments(global_caps, start, hint)
            if not cap_segs and scene.get("narration"):
                cap_text = tts_text(scene)
                if cap_text:
                    caption = cap_text
        html, dur = gen_scene_html(scene, start, idx, caption=caption,
                                   registry=registry, css_out=tpl_css,
                                   caption_segments=cap_segs)
        if cap_segs:
            cap_by_scene[idx] = cap_segs
        scene_html_parts.append(html)
        durs.append(dur)
        trans.append(ttype)
        start += dur
    total_duration = start

    # GSAP 时间线（含分段字幕的点亮/熄灭 tween）
    timeline_js = gen_timeline_js(scenes, starts, durs, trans, trans_durs,
                                  cap_by_scene)

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

    style_block = "\n".join(scene_styles)
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
       data-height="{height}" data-time-unit="s" data-fps="{fps}">
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
