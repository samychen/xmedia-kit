#!/usr/bin/env python3
# xmedia-kit Agent 指令层：自然语言 → spec 变更 / 管线动作。
#
# 两级设计（无密钥也能跑完整闭环）：
#   1. 本地规则引擎：常用中文/英文编辑指令直接改 spec，确定性、零依赖。
#   2. 可选 LLM：设置 XMEDIA_LLM_ENDPOINT（OpenAI 兼容 /chat/completions）、
#      XMEDIA_LLM_API_KEY、XMEDIA_LLM_MODEL 后，规则未命中时转发给模型，
#      要求模型只回 JSON {"reply", "spec"}；密钥只在服务端，不下发浏览器。
#
# 任何来源的 spec 都必须过 validate_spec() 才允许写回——对应
# docs/HF_COMPOSE.md 里「agent 不手写 HTML、按 schema 填 spec JSON」的约定。
#
# 也可独立 CLI 自检：hf_agent.py "加一个片尾卡" < spec.json

import argparse
import copy
import json
import os
import re
import sys
import urllib.request

MAX_SPEC_BYTES = 200_000
MAX_SCENES = 100
SYSTEM_PROMPT = """你是 xmedia-kit Studio 的视频创作与剪辑 Agent。用户给你自然语言指令、当前 spec JSON、界面上下文（当前选中镜头索引/播放时间/画面圈选区域）、可用设计预设列表、本地素材列表与近期对话历史。
只输出一个合法 JSON 对象（不要 markdown 代码块、不要多余解释文字）：
1. 如果用户在询问方案、头脑风暴、大纲策划、或提问：不修改 spec，返回：
{"reply": "清晰简洁的中文方案或回答", "choices": [{"label": "方向名", "desc": "一句话卖点", "msg": "点击后可执行的指令"}], "suggestions": ["可点击的下一步短指令1", "短指令2"]}
2. 如果用户要求新建、编辑或优化视频：输出修改后的完整 spec，返回：
{"reply": "一两句平实中文说明修改了屏幕上什么", "spec": {完整的新 spec JSON}, "suggestions": ["可点击的后续建议1", "建议2"]}

spec 规则：顶层 schema_version 固定 "1.0"；可选顶层 design 填入 available_presets 之一（如 midnight-glow / editorial-stone / blue-professional / editorial-forest / coral-pop / code-editorial / bold-poster）；composition 含 format(16:9/9:16/1:1/4:5) 与 fps；
scenes 是非空数组，每项含 type(title/text/outro/quote/list/stats/comparison 或 available_templates 中的模板名)、可选 kicker(眉标标签)、
text(或 title/quote/value/left/right)、duration(秒，>0)、bg(十六进制色)、可选 subtitle/text_color/image({src,mode})/animation(fade_in/fade_in_up/fade_in_down/slide_in_left/slide_in_right/scale_in/zoom_in/zoom_out/pop_in/blur_in/clip_reveal/none)/transition(fade/slide-left/slide-right/wipe/zoom/blur/none)/video/narration/captions；
顶层可选 captions:{enabled,segments:[{start,end,text}]}、audio:{bgm:{src,volume}}、sfx:{enabled}、transitions:{type,duration}。
遵循当前品牌规则与叙事约束；不要写 HTML、不要写 JavaScript、不要编造不存在的本地素材路径（只能使用 available_media 里列出的路径）。"""


class AgentRejected(RuntimeError):
    """LLM 返回的 spec 不合法，拒绝写回。"""


class LlmUnavailable(RuntimeError):
    """未配置 LLM 或调用失败。"""


def available_types(spec_dir=""):
    import hf_compose
    allowed = set(hf_compose.SUPPORTED_TYPES)
    try:
        allowed |= set(hf_compose.load_custom_templates(spec_dir or ".").keys())
    except OSError:
        pass
    return allowed


def validate_spec(spec, spec_dir=""):
    """Agent 产出的 spec 硬门：结构、schema、场景类型、规模、素材路径。"""
    if not isinstance(spec, dict):
        raise AgentRejected("spec 必须是 JSON 对象")
    if len(json.dumps(spec, ensure_ascii=False).encode("utf-8")) > MAX_SPEC_BYTES:
        raise AgentRejected("spec 过大（>200KB）")
    scenes = spec.get("scenes")
    if not isinstance(scenes, list) or not scenes:
        raise AgentRejected("scenes 必须是非空数组")
    if len(scenes) > MAX_SCENES:
        raise AgentRejected(f"场景数超过上限 {MAX_SCENES}")
    allowed = available_types(spec_dir)
    for i, sc in enumerate(scenes):
        if not isinstance(sc, dict):
            raise AgentRejected(f"scenes[{i}] 必须是对象")
        typ = str(sc.get("type", "text"))
        if typ not in allowed:
            raise AgentRejected(
                f"scenes[{i}].type '{typ}' 不在支持列表/模板目录")
        try:
            dur = float(sc.get("duration", 3))
        except (TypeError, ValueError):
            raise AgentRejected(f"scenes[{i}].duration 不是数字")
        if dur <= 0 or dur > 3600:
            raise AgentRejected(f"scenes[{i}].duration 超出 0~3600 秒")
        for key in ("video", "narration"):
            val = sc.get(key)
            if val and not isinstance(val, str):
                raise AgentRejected(f"scenes[{i}].{key} 必须是路径字符串")
            if isinstance(val, str) and val.lower().startswith(
                    ("javascript:", "data:", "http://", "https://")):
                raise AgentRejected(f"scenes[{i}].{key} 不允许远程/脚本 URL")
    audio = spec.get("audio") or {}
    if audio and not isinstance(audio, dict):
        raise AgentRejected("audio 必须是对象")
    caps = spec.get("captions") or {}
    if not isinstance(caps, dict):
        # 场景级 captions 是数组，顶层必须是对象
        raise AgentRejected("captions 必须是对象")
    if isinstance(caps.get("segments"), list):
        for seg in caps["segments"][:800]:
            if not isinstance(seg, dict) or "text" not in seg:
                raise AgentRejected("captions.segments 元素缺 text")
    # schema_version 由生成器钉死，避免 LLM 改成不支持的版本导致 compose 失败
    return True


def _list_media_files(spec_dir):
    out = []
    if not spec_dir or not os.path.isdir(spec_dir):
        return out
    exts = {".jpg", ".jpeg", ".png", ".webp", ".svg", ".gif",
            ".mp4", ".mov", ".webm", ".mp3", ".wav", ".m4a"}
    base_depth = spec_dir.rstrip(os.sep).count(os.sep)
    for cur, dirs, files in os.walk(spec_dir):
        dirs[:] = sorted(d for d in dirs if not d.startswith(".")
                         and d not in ("__thumbs__", "node_modules"))
        if cur.count(os.sep) - base_depth > 2:
            dirs[:] = []
            continue
        for fn in sorted(files):
            if os.path.splitext(fn)[1].lower() in exts:
                out.append(os.path.relpath(os.path.join(cur, fn), spec_dir))
                if len(out) >= 40:
                    return out
    return out


def _llm_complete(message, spec, context=None, history=None, templates=None,
                  spec_dir="", llm_config=None, timeout=60):
    cfg = llm_config if isinstance(llm_config, dict) else {}
    endpoint = str(cfg.get("endpoint") or os.environ.get(
        "XMEDIA_LLM_ENDPOINT", "")).strip()
    if not endpoint:
        raise LlmUnavailable("未配置大模型接口地址")
    key = str(cfg.get("api_key") or os.environ.get(
        "XMEDIA_LLM_API_KEY", "")).strip()
    model = str(cfg.get("model") or os.environ.get(
        "XMEDIA_LLM_MODEL", "gpt-4o-mini")).strip() or "gpt-4o-mini"
    presets_info = []
    try:
        import hf_compose
        for k, v in sorted(hf_compose._design_presets().items()):
            presets_info.append({"name": k, "desc": v.get("description", "")})
    except Exception:
        pass
    user_obj = {
        "instruction": message,
        "current_spec": spec,
        "context": context or {},
        "available_templates": templates or [],
        "available_presets": presets_info,
        "available_media": _list_media_files(spec_dir),
        "recent_history": (history or [])[-5:],
    }
    payload = {"model": model, "temperature": 0.2, "messages": [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": json.dumps(
            user_obj, ensure_ascii=False)}]}
    headers = {"Content-Type": "application/json"}
    if key:
        headers["Authorization"] = "Bearer " + key
    req = urllib.request.Request(
        endpoint, data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = json.load(r)
    except Exception as e:
        raise LlmUnavailable(f"LLM 调用失败：{e}")
    try:
        content = body["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        raise LlmUnavailable("LLM 响应缺少 choices[0].message.content")
    content = re.sub(r"^```(?:json)?\s*|\s*```$", "", content.strip())
    try:
        data = json.loads(content)
    except json.JSONDecodeError as e:
        raise LlmUnavailable(f"LLM 未返回合法 JSON：{e}")
    if not isinstance(data, dict) or ("spec" not in data and
                                      "reply" not in data):
        raise LlmUnavailable("LLM 响应缺少 reply/spec 字段")
    return data


HELP = ("可用指令：导入 <目录> / 扫描 <网址> 做成品牌 / 应用品牌 <名> / "
        "加片尾卡 / 删除第 N 个场景 / "
        "第 N 个场景文字改成 X / 改成竖屏 / 背景改成 #1a1a2e / "
        "关闭字幕 / 生成旁白 / 自动字幕(ASR) / 导出 MP4 / 第 N 个场景时长 X 秒。"
        " 自由自然语言：切到 agent 后端（顶栏 → agent，本机 CodeBuddy/"
        "claude CLI 驱动）即可说任意表述。")


# 单个路径段只认 ASCII 字符，避免把中文正文（「我录制的视频」）吞进路径；
# 中文 / 带空格的目录请用引号包裹。
_SEG = r"[A-Za-z0-9._\-]"


def _looks_path(s):
    """判断一段文本像不像素材路径，避免把「我录制的视频」这类正文当目录。"""
    s = (s or "").strip()
    if not s:
        return False
    if s.startswith(("~", "/", "./", "../")):
        return True
    if "/" in s:
        return True
    try:
        return os.path.isdir(os.path.expanduser(s))
    except Exception:
        return False


def _extract_path(msg):
    """从自然语言里抽出一个素材路径。支持：引号包裹（可含空格 / 中文）、
    `~/`、绝对路径、`./` 或含 `/` 的相对路径。找不到返回空串。"""
    # 1) 引号包裹优先（允许内部空格、中文路径）
    m = re.search(r'["\'“”‘’`]([^"\'“”‘’`]{1,300}?)["\'“”‘’`]', msg)
    if m and _looks_path(m.group(1)):
        return m.group(1).strip()
    # 2) ~/a/b 或 ~user/a：`~/` 已足够明确是路径，允许中文目录名
    m = re.search(r"(~[^\s，,。；;：:！!？?、|]*)", msg)
    if m and _looks_path(m.group(1)):
        return m.group(1)
    # 3) ./a 或 ../a（须在绝对路径之前，避免 "./clips" 被截成 "/clips"）
    m = re.search(r"((?:\.\.?/)(?:" + _SEG + r"+/)*" + _SEG + r"+)", msg)
    if m and _looks_path(m.group(1)):
        return m.group(1)
    # 4) 绝对路径 /a/b/c
    m = re.search(r"(/(?:" + _SEG + r"+/)*" + _SEG + r"+)", msg)
    if m and _looks_path(m.group(1)):
        return m.group(1)
    # 5) 相对路径 a/b （含 /，无前导符号）
    m = re.search(r"(" + _SEG + r"+(?:/" + _SEG + r"+)+)", msg)
    if m and _looks_path(m.group(1)):
        return m.group(1)
    # 6) 裸文件名（必须带音视频扩展名，避免把正文当路径）：kpl.mp4 / cf原视频.mp4
    m = re.search(r"([^\s，,。；;：:！!？?、|\"'“”‘’]+\.(?:mp4|mov|mkv|avi|"
                  r"webm|m4v|wav|mp3|m4a|aac|flac|ogg))", msg, re.I)
    if m:
        return m.group(1)
    return ""


def _topic_of(msg):
    """从「参考 <路径> 讲 X」之类消息里抽主题短语；抽不到返回空。"""
    m = re.search(r"(?:讲|主题|话题|内容)\s*[:：]?\s*(.{2,40})", msg)
    return m.group(1).strip() if m else ""


def apply_brand(spec, brand):
    """品牌套件统一注入（xmedia brand apply / agent 指令 / Home 默认
    品牌共用这一份逻辑）。v2 结构（colors with roles / ink / fonts /
    rules）为主，v1 扁平字段（palette/text_color）兼容读取。

    返回 (spec, 摘要行列表)。"""
    scenes = spec.get("scenes") or []
    # 色板：v2 colors[{hex, role}] 按序循环；v1 palette 兼容
    colors = [c["hex"] for c in brand.get("colors", []) if c.get("hex")]
    if not colors:
        colors = brand.get("palette", [])
    text = brand.get("ink") or brand.get("text_color", "")
    n = 0
    for i, sc in enumerate(scenes):
        if not sc.get("video"):
            if colors:
                sc["bg"] = colors[i % len(colors)]
            if text:
                sc.setdefault("text_color", text)
                sc.setdefault("color", text)
            n += 1
    if spec.get("background"):
        spec["background"] = brand.get("background", spec["background"])
    tts_v = brand.get("tts_voice") or (
        brand["voice"] if isinstance(brand.get("voice"), str) else "")
    if tts_v:
        spec.setdefault("tts", {})["voice"] = tts_v
    # 字体：v2 fonts.body.family；v1（brand save --font-family /
    # brand scan 产物）fonts 是 font-family 字符串数组，取首个。
    fonts = brand.get("fonts") or {}
    if isinstance(fonts, list):
        body_font = (fonts[0] if fonts else "") or \
            brand.get("font_family", "")
    else:
        body_font = (fonts.get("body") or {}).get("family") or \
            brand.get("font_family", "")
    if body_font:
        spec.setdefault("fonts", {})["body"] = body_font
    # outro
    for sc in scenes:
        if sc.get("type") == "outro":
            if brand.get("outro_text"):
                sc["text"] = brand["outro_text"]
            logo_text = brand.get("logo_text") or \
                (brand.get("outro") or {}).get("logo_text", "")
            if logo_text and "subtitle" in sc:
                sc["subtitle"] = logo_text
    # logo 图片（identity mark）：brand["logo_src"] 由调用方解析为
    # 对 spec/契约页可读的 src（相对项目目录或绝对路径）。
    logo_src = brand.get("logo_src") or brand.get("logo_image") or ""
    mode = brand.get("logo_mode", "corner")
    n_logo = 0
    if logo_src:
        for sc in scenes:
            if not sc.get("image") and not sc.get("video"):
                sc["image"] = {"src": logo_src, "mode": mode}
                n_logo += 1
    notes = [f"色板 {len(colors) or '继承'} 色 · 文字 {text or '不变'}"
             f" · 音色 {brand.get('voice') or '不变'}"
             f" · logo 图 {n_logo} 处"]
    return spec, notes


def _int(s, default=0):
    cn = {"一": 1, "二": 2, "三": 3, "四": 4, "五": 5,
          "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}
    s = (s or "").strip()
    if s.isdigit():
        return int(s)
    return cn.get(s, default)


def _default_suggestions(spec):
    """根据当前 spec 缺口推荐最多 3 个下一步快捷建议。"""
    if not isinstance(spec, dict):
        return []
    scenes = spec.get("scenes") or []
    sugs = []
    has_video = any(sc.get("video") for sc in scenes)
    has_narration = any(sc.get("narration") for sc in scenes)
    has_caps = bool((spec.get("captions") or {}).get("segments"))
    has_outro = any(str(sc.get("type")) in ("outro", "outro-brand")
                    for sc in scenes)
    if has_video and not has_caps:
        sugs.append("自动字幕")
    if scenes and not has_narration:
        sugs.append("生成旁白")
    if scenes and not has_outro:
        sugs.append("加一个片尾卡")
    if len(scenes) >= 2 and not any(sc.get("transition") for sc in scenes[1:]):
        sugs.append("全部转场改成 fade")
    return sugs[:3]


def _resolve_deictic(msg, scenes, context):
    """将「这镜/当前场景/这里」等指代词替换为明确的「第 N 个场景」。"""
    if not isinstance(context, dict):
        return msg, None
    idx = context.get("scene_index")
    if isinstance(idx, bool) or not isinstance(idx, int):
        return msg, None
    if idx < 0 or idx >= len(scenes):
        return msg, None
    if re.search(r"第\s*[一二三四五六七八九十\d]+\s*个?\s*(?:场景|镜)", msg):
        return msg, idx
    replaced = re.sub(
        r"(?:当前|这|本|该)\s*(?:一\s*)?(?:个\s*)?(?:场景|镜|幕)",
        f"第 {idx + 1} 个场景",
        msg,
    )
    return replaced, idx


def _apply_design_preset(spec, preset_name):
    """套用内置设计预设到 spec（支持 7 套 presets）。"""
    import hf_compose
    presets = hf_compose._design_presets()
    p = presets.get(preset_name)
    if not p:
        return None
    colors = p.get("colors") or {}
    bg1 = colors.get("bg_primary") or "#0a0a1a"
    bg2 = colors.get("bg_secondary") or bg1
    ink = colors.get("text_primary") or "#ffffff"
    spec["design"] = preset_name
    spec["background"] = bg1
    for i, sc in enumerate(spec.get("scenes") or []):
        if not sc.get("video"):
            sc["bg"] = bg1 if (i % 2 == 0) else bg2
            sc["text_color"] = ink
    return p


def _match_preset_name(msg):
    import hf_compose
    presets = sorted(hf_compose._design_presets().keys())
    low = msg.lower()
    for name in presets:
        if name in low:
            return name
    aliases = [
        (r"暖石|米色|极简|serif|编辑风|浅色|亮色|太暗|看不清", "editorial-stone"),
        (r"蓝白|专业|商务|企业|钴蓝", "blue-professional"),
        (r"霓虹|暗夜|科技感|赛博|极客|saas", "midnight-glow"),
        (r"森林|绿|文艺|自然|有机", "editorial-forest"),
        (r"珊瑚|撞色|活力|潮流|红黑", "coral-pop"),
        (r"工程|代码|暖纸|出版", "code-editorial"),
        (r"海报|反白|黑底白字|冲击", "bold-poster"),
    ]
    for pat, target in aliases:
        if target in presets and re.search(pat, msg, re.I):
            return target
    return None


def _build_zero_config_video(topic, fmt="16:9", preset_name="midnight-glow",
                            arc="launch"):
    import hf_compose
    import hf_storyboard
    presets = hf_compose._design_presets()
    if preset_name not in presets:
        preset_name = "midnight-glow" if "midnight-glow" in presets else (
            next(iter(presets), ""))
    spec = hf_storyboard.build(arc, topic, fmt=fmt, voice="Tingting")
    if arc == "comparison":
        scenes = [
            {"type": "title", "kicker": "COMPARISON", "text": topic[:48],
             "subtitle": "新旧方案核心差异一眼看清", "duration": 3.2,
             "animation": "fade_in_up"},
            {"type": "comparison", "kicker": "SIDE BY SIDE",
             "text": f"{topic[:18]} 前后对比",
             "left": "传统方式", "right": "全新方案",
             "left_items": ["流程繁琐耗时", "手工重复校对", "效果难以复现"],
             "right_items": ["秒级自动生成", "统一视觉规范", "确定性稳定出片"],
             "duration": 4.2, "animation": "slide_in_left", "transition": "fade"},
            {"type": "stats", "kicker": "EFFICIENCY", "value": "10×",
             "description": f"{topic[:20]} 整体效率显著提升",
             "duration": 3.0, "animation": "pop_in", "transition": "wipe"},
            {"type": "outro", "text": f"立即体验 {topic[:20]}",
             "duration": 2.6, "animation": "fade_in", "transition": "fade"},
        ]
    elif arc == "listicle":
        scenes = [
            {"type": "title", "kicker": "TOP PICKS", "text": topic[:48],
             "subtitle": "最值得关注的三个核心亮点", "duration": 3.2,
             "animation": "fade_in_up"},
            {"type": "list", "kicker": "HIGHLIGHTS", "text": "核心亮点速览",
             "items": [f"亮点 01 · {topic[:14]} 极速上手",
                       "亮点 02 · 结构化高质感视觉",
                       "亮点 03 · 实时预览即改即出"],
             "duration": 4.2, "animation": "slide_in_left", "transition": "fade"},
            {"type": "stats", "kicker": "RESULT", "value": "100%",
             "description": "所见即所得，全流程本地可控",
             "duration": 3.0, "animation": "zoom_in", "transition": "slide-left"},
            {"type": "outro", "text": f"开启你的 {topic[:18]}",
             "duration": 2.6, "animation": "fade_in", "transition": "fade"},
        ]
    else:
        scenes = [
            {"type": "title", "kicker": "INTRODUCING", "text": topic[:48],
             "subtitle": "重新定义高效创作与确定性体验", "duration": 3.5,
             "animation": "fade_in_up"},
            {"type": "text", "kicker": "THE CHALLENGE",
             "text": f"为什么你需要 {topic[:22]}？",
             "content": [{"text": "告别繁琐手工搭建与样式失控"},
                         {"text": "让每个想法直接落地成高质感成片"}],
             "duration": 3.6, "animation": "fade_in", "transition": "fade"},
            {"type": "list", "kicker": "KEY FEATURES", "text": "核心能力一览",
             "items": ["开箱即用 · 语义直接驱动画面结构",
                       "品牌一致 · 网站扫码直连视觉规范",
                       "毫秒响应 · 随时改镜重排无等待"],
             "duration": 4.0, "animation": "slide_in_left", "transition": "wipe"},
            {"type": "stats", "kicker": "BY THE NUMBERS", "value": "10×",
             "description": f"用 {topic[:16]} 让创意到交付快人十倍",
             "duration": 3.0, "animation": "pop_in", "transition": "slide-left"},
            {"type": "outro", "text": f"立即尝试 {topic[:22]}",
             "duration": 2.8, "animation": "fade_in", "transition": "fade"},
        ]
    spec["scenes"] = scenes
    spec["transitions"] = {"type": "fade", "duration": 0.45}
    if preset_name:
        _apply_design_preset(spec, preset_name)
    return spec


def handle(message, spec, spec_dir="", context=None, history=None,
           llm_config=None):
    """→ {reply, spec?, action?, args?, mode, suggestions?, choices?}。"""
    raw_msg = str(message or "").strip()
    base = copy.deepcopy(spec) if isinstance(spec, dict) else {}
    if not base.get("scenes"):
        base = {"schema_version": "1.0",
                "composition": {"id": "main", "format": "16:9", "fps": 30},
                "scenes": [{"type": "text", "text": "", "duration": 3}]}
    msg, ctx_idx = _resolve_deictic(raw_msg, base["scenes"], context)

    def out(reply, new_spec=None, action=None, args=None, mode="rule",
            suggestions=None, choices=None):
        payload = {"reply": reply, "mode": mode}
        if new_spec is not None:
            new_spec = copy.deepcopy(new_spec)
            new_spec["schema_version"] = "1.0"
            payload["spec"] = new_spec
            sugs = (suggestions if suggestions is not None
                    else _default_suggestions(new_spec))
            if sugs:
                payload["suggestions"] = sugs
        elif suggestions:
            payload["suggestions"] = suggestions
        if choices:
            payload["choices"] = choices[:4]
        if action:
            payload["action"] = action
            payload["args"] = args or {}
        return payload

    # 0. Launch 向导引导（对齐 HF first-video 的四段式卡片）：CLI 落地，
    #    聊天内给模板让用户复制填写。
    if re.search(r"launch|四段|hook.*proof|发布视频向导", msg, re.I):
        return out(
            "四段式 launch 向导（对齐 HF first-video：Hook → Proof → "
            "Turn → CTA）。复制下面命令填写后运行，然后在 Studio 打开：\n\n"
            "python3 tools/xmedia.py launch \\\n"
            "  --hook \"一句话钩子\" \\\n"
            "  --proof \"数据|说明\" \\\n"
            "  --turn \"差异化主张\" \\\n"
            "  --cta \"行动号召\" \\\n"
            "  --url https://你的网站 --out <目录>/launch.json\n\n"
            "可选：--idea 副标题 / --include-site 网站要点入 list 场景 / "
            "--format 16:9 或 1:1 / --voice 音色。生成后直接 "
            "xmedia render 出片。", mode="help")

    # 0b. 品牌套件（brand kit）：~/.xmedia/brands/<名>.json 注入当前 spec。
    m = re.search(r"应用品牌\s*[:：]?\s*([\w\-]+)", msg)
    if m:
        import json as _json
        bpath = os.path.expanduser(os.environ.get(
            "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
        fpath = os.path.join(bpath, m.group(1) + ".json")
        if not os.path.exists(fpath):
            names = sorted(f[:-5] for f in os.listdir(bpath)
                           if f.endswith(".json")) if os.path.isdir(bpath) else []
            return out(f"品牌「{m.group(1)}」不存在。已保存的品牌："
                       + ("、".join(names) or "（无）——先跑 xmedia brand save"),
                       mode="help")
        brand = _json.load(open(fpath, encoding="utf-8"))
        palette = brand.get("palette", [])
        text = brand.get("text_color", "")
        for i, sc in enumerate(base.get("scenes", [])):
            if not sc.get("video"):
                if palette:
                    sc["bg"] = palette[i % len(palette)]
                if text:
                    sc.setdefault("color", text)
        if base.get("background"):
            base["background"] = brand.get("background", base["background"])
        tts_v = brand.get("tts_voice") or (
            brand["voice"] if isinstance(brand.get("voice"), str) else "")
        if tts_v:
            base.setdefault("tts", {})["voice"] = tts_v
        for sc in base.get("scenes", []):
            if sc.get("type") == "outro":
                if brand.get("outro_text"):
                    sc["text"] = brand["outro_text"]
                if brand.get("logo_text") and "subtitle" in sc:
                    sc["subtitle"] = brand["logo_text"]
        return out(f"品牌「{m.group(1)}」已应用（色板 {len(palette)} 色、"
                   f"文字 {text or '不变'}、音色 "
                   f"{tts_v or '不变'}）", base)

    # 0b-2. 扫描网站 → 品牌（HF brand kits 的「输入网址生成 kit」形态）：
    #     消息带 URL + 品牌词 → xmedia brand scan（抓页面 + 至多 3 个同源
    #     CSS 统计色板/字体/logo，存 ~/.xmedia/brands/<名>.json）→
    #     apply_brand 注入当前 spec。品牌名缺省取域名主体。
    m_url = re.search(r"((?:https?|file)://[^\s\"'<>」』】]+)", msg)
    if m_url and re.search(
            r"扫描|做成品牌|生成品牌|品牌套件|品牌指南|识别品牌|"
            r"brand ?kit|brand.?scan|配色|色板", msg, re.I):
        import subprocess as _sp
        url = m_url.group(1)
        m_name = re.search(
            r"品牌\s*(?:叫|名叫|名为|名字是|存成|保存成|命名为)"
            r"\s*[为:：]?\s*([\w\-]{2,24})", msg, re.I)
        if m_name:
            name = m_name.group(1)
        elif url.startswith("file://"):
            name = os.path.splitext(os.path.basename(
                url.split("://", 1)[-1].split("?")[0]))[0] or "scanned"
        else:
            host = url.split("//", 1)[-1].split("/", 1)[0]
            host = host.split(":")[0].lower()
            if host.startswith("www."):
                host = host[4:]
            name = host.split(".")[0] or "scanned"
        bpath = os.path.expanduser(os.environ.get(
            "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))
        fpath = os.path.join(bpath, name + ".json")
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        try:
            r = _sp.run(
                [sys.executable, os.path.join(root, "tools", "xmedia.py"),
                 "brand", "scan", name, "--url", url],
                capture_output=True, text=True, timeout=60, cwd=root)
        except _sp.TimeoutExpired:
            return out("扫描超时（网站响应太慢）。稍后再试，或直接说"
                       f"「应用品牌 {name}」（若之前扫过）。", mode="help")
        if r.returncode != 0 or not os.path.exists(fpath):
            tail = [ln for ln in (r.stderr or r.stdout or "").strip()
                    .splitlines() if ln.strip()]
            return out("扫描失败：" + (tail[-1] if tail else "未知错误"),
                       mode="help")
        brand = json.load(open(fpath, encoding="utf-8"))
        li = brand.get("logo_image")
        if li and not os.path.isabs(li):
            cand = os.path.join(bpath, li)
            if os.path.exists(cand):
                brand["logo_src"] = cand
        base, notes = apply_brand(base, brand)
        palette = brand.get("palette", [])
        fonts = brand.get("fonts") or []
        reply = (f"品牌「{name}」已从 {url} 扫描生成并应用到当前项目：\n"
                 f"· 色板 {' '.join(palette)}"
                 f"（背景 {brand.get('background')}"
                 f" · 文字 {brand.get('text_color')}）\n"
                 + (f"· 字体 {'、'.join(fonts)}\n" if fonts else "")
                 + f"· {notes[0] if notes else '已注入'}\n"
                 f"之后随时说「应用品牌 {name}」即可再套用。")
        return out(reply, base)

    # 0c. 参考视频（HF reference-video 的本地弱化版指令入口）：路径 →
    #     action 分发，Studio 侧跑 hf_reference（镜头切割 → 同节奏 spec
    #     骨架 + 可选 ASR），复刻剪辑节奏与结构，不复刻特效/素材。
    m = re.search(r"参考\s*(?:视频)?\s*[:：]?\s*(\S+)", msg)
    if m:
        raw = m.group(1).strip("\"'“”")
        path = os.path.expanduser(raw)
        if not _looks_path(raw):
            return out("请提供参考视频路径，例如「参考 ~/Movies/demo.mp4 "
                       "讲 XX」或「参考 ./ref.mp4」", mode="help")
        return out(f"正在分析参考视频 {path}（镜头切割 → 同节奏 spec 骨架"
                   "，可选 ASR 字幕）…", action="reference",
                   args={"video": path, "topic": _topic_of(msg)})

    # 1. 素材导入（含「字幕」「片尾卡」组合意图）
    #    只把「像路径」的片段当路径；没有路径时给出引导而不是抛「不存在」。
    if re.search(r"导入|import", msg, re.I):
        raw_path = _extract_path(msg)
        if not raw_path:
            return out("请提供素材路径，例如「导入 ~/Movies/kpl.mp4」或"
                       "「导入 ~/Downloads/clips」，也可以点「导入素材」"
                       "按钮选择文件 / 文件夹。", mode="help")
        path = os.path.expanduser(raw_path)
        return out(f"正在导入素材 {path} …", action="import", args={
            "dir": path,
            "asr": bool(re.search(r"字幕|caption|转写|ASR", msg, re.I)),
            "endcard": bool(re.search(r"片尾|endcard|outro", msg, re.I)),
            "captions": ""})

    # 1b. 没说「导入」但明显是「把某素材放到时间轴」：带路径 + 放/加/拼等词
    if re.search(r"时间轴|放到|加到|放进|拼到|接上|拼接|串起来", msg):
        raw_path = _extract_path(msg)
        if raw_path:
            path = os.path.expanduser(raw_path)
            return out(f"正在导入素材 {path} …", action="import", args={
                "dir": path,
                "asr": bool(re.search(r"字幕|caption|转写|ASR", msg, re.I)),
                "endcard": bool(re.search(r"片尾|endcard|outro", msg, re.I)),
                "captions": ""})

    # 2. 片尾卡 / endcard
    if re.search(r"片尾卡|品牌收尾|endcard|add (?:our )?outro|片尾", msg, re.I):
        if any(str(s.get("type")) == "outro-brand" for s in base["scenes"]):
            return out("已经有品牌片尾卡了，没有重复添加。")
        base["scenes"].append({
            "type": "outro-brand", "brand": "xmedia-kit",
            "tagline": "Made with xmedia-kit", "duration": 2.5,
            "bg": "#0f3460"})
        return out("已追加品牌片尾卡（outro-brand 模板）", base)

    # 3. 删除场景
    m = re.search(r"(?:删除|删掉|移除|remove|delete)\s*第?\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|scene)", msg, re.I)
    if m:
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(base["scenes"]):
            base["scenes"].pop(idx)
            return out(f"已删除第 {idx+1} 个场景，剩余 {len(base['scenes'])} 个", base)
        return out(f"没有第 {idx+1} 个场景（共 {len(base['scenes'])} 个）")

    # 4. 改场景文字
    # 8b. 副标题 / 单场景背景 / 单场景文字色（高频字段逐场景编辑）
    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,6}(?:眉标|kicker|标签)\s*(?:改成|改为|设为|清空)\s*[:：]?\s*(.*)", msg, re.I)
    if m:
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(base["scenes"]):
            val = m.group(2).strip().strip("\"'“”")
            if val:
                base["scenes"][idx]["kicker"] = val
            else:
                base["scenes"][idx].pop("kicker", None)
            return out(f"第 {idx+1} 个场景眉标已更新为「{val or '无'}」", base)

    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,6}副标题.{0,6}(?:改成|改为|设为|清空)\s*[:：]?\s*(.*)", msg, re.I)
    if m:
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(base["scenes"]):
            base["scenes"][idx]["subtitle"] = m.group(2).strip().strip("\"'“”")
            return out(f"第 {idx+1} 个场景副标题已改为「{base['scenes'][idx]['subtitle']}」",
                       base)

    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,8}(?:文字|文案|标题|text)\s*(?:改成|改为|设为|set to|to)\s*[:：]?\s*(.+)", msg, re.I)
    if m:
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(base["scenes"]):
            base["scenes"][idx]["text"] = m.group(2).strip().strip("\"'“”")
            return out(f"第 {idx+1} 个场景文字已改为「{base['scenes'][idx]['text']}」", base)

    # 4b. 字幕段文字编辑（Script 逐词面板的 Cut/Fix 落点）：改
    #     captions.segments[N-1].text——注意 TTS 音频由 tts_text 生成，
    #     这里只改字幕显示文本，剪音频请改旁白文本后重新生成旁白。
    m = re.search(r"字幕段\s*([一二三四五六七八九十\d]+)\s*(?:的)?(?:文字|文本)?\s*(?:改成|改为|设为)\s*[:：]?\s*(.+)", msg, re.I)
    if m and not re.search(r"关闭字幕|去掉字幕|删除字幕", msg):
        caps = base.setdefault("captions", {})
        segs = caps.setdefault("segments", [])
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(segs):
            new_text = m.group(2).strip().strip("\"'“”")
            old = segs[idx].get("text", "")
            segs[idx]["text"] = new_text
            caps["enabled"] = True
            return out(f"字幕段 {idx+1} 已改为「{new_text}」"
                       f"（原「{old}」；音频未剪，剪音频请改旁白后重新生成）",
                       base)
        return out(f"字幕段 {idx+1} 不存在（当前 {len(segs)} 段）", mode="help")

    # 5. 画幅
    if re.search(r"竖屏|9:16|9：16", msg, re.I):
        base.setdefault("composition", {})["format"] = "9:16"
        return out("画幅已改为 9:16 竖屏（1080×1920）", base)
    if re.search(r"横屏|16:9|16：9", msg, re.I):
        base.setdefault("composition", {})["format"] = "16:9"
        return out("画幅已改为 16:9 横屏（1920×1080）", base)

    # 6. 背景色
    m = re.search(r"背景.{0,6}?(#[0-9a-fA-F]{3,8})", msg)
    if m:
        color = m.group(1)
        for sc in base["scenes"]:
            if not sc.get("video"):
                sc["bg"] = color
        return out(f"无实拍素材的场景背景已改为 {color}", base)
    if re.search(r"深色背景|暗色背景", msg):
        for sc in base["scenes"]:
            if not sc.get("video"):
                sc["bg"] = "#0a0a1a"
        return out("已改为深色背景 #0a0a1a", base)

    # 7. 字幕开关
    if re.search(r"关闭字幕|去掉字幕|隐藏字幕|hide captions|字幕关", msg, re.I):
        base.setdefault("captions", {})["enabled"] = False
        return out("已关闭字幕轨", base)
    if re.search(r"开启字幕|显示字幕|show captions|字幕开", msg, re.I):
        base.setdefault("captions", {})["enabled"] = True
        return out("已开启字幕轨", base)

    # 8. 场景时长
    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,6}时长\s*([\d.]+)\s*秒", msg)
    if m:
        idx, dur = _int(m.group(1), 0) - 1, float(m.group(2))
        if 0 <= idx < len(base["scenes"]) and 0 < dur <= 3600:
            base["scenes"][idx]["duration"] = dur
            return out(f"第 {idx+1} 个场景时长改为 {dur}s", base)

    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,4}背景.{0,4}?(#[0-9a-fA-F]{3,8})", msg)
    if m:
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(base["scenes"]):
            base["scenes"][idx]["bg"] = m.group(2)
            return out(f"第 {idx+1} 个场景背景已改为 {m.group(2)}", base)
    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,4}(?:文字色|字色).{0,4}?(#[0-9a-fA-F]{3,8})", msg)
    if m:
        idx = _int(m.group(1), 0) - 1
        if 0 <= idx < len(base["scenes"]):
            base["scenes"][idx]["text_color"] = m.group(2)
            return out(f"第 {idx+1} 个场景文字色已改为 {m.group(2)}", base)

    # 8c. 转场 / 入场动画（场景级，改完即生效）
    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,6}转场\s*(?:改成|改为|设为)?\s*[:：]?\s*(\S+)", msg, re.I)
    if m:
        idx, val = _int(m.group(1), 0) - 1, m.group(2).strip().lower()
        if val in ("fade", "slide-left", "slide-right", "wipe", "zoom", "blur", "none"):
            if 0 <= idx < len(base["scenes"]):
                base["scenes"][idx]["transition"] = val
                return out(f"第 {idx+1} 个场景转场已改为 {val}", base)
    m = re.search(r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,6}(?:动画|入场动画)\s*(?:改成|改为|设为)?\s*[:：]?\s*(\S+)", msg, re.I)
    if m:
        idx, val = _int(m.group(1), 0) - 1, m.group(2).strip()
        if val in ("fade_in", "fade_in_up", "fade_in_down", "scale_in",
                   "slide_in", "slide_in_left", "slide_in_right",
                   "zoom_in", "zoom_out", "pop_in", "blur_in", "clip_reveal",
                   "none"):
            if 0 <= idx < len(base["scenes"]):
                base["scenes"][idx]["animation"] = val
                return out(f"第 {idx+1} 个场景入场动画已改为 {val}", base)

    # 8d. 插入场景到指定位置（第 N 位前）：text 场景占位，字段随后编辑
    m = re.search(r"(?:在\s*)?第?\s*([一二三四五六七八九十\d]+)\s*个?场景(?:之前|前面|位置)\s*插入.{0,8}场景[，,]?\s*(?:文字|文案)?\s*[:：]?\s*(.*)", msg)
    if m:
        pos = _int(m.group(1), 0) - 1
        pos = max(0, min(pos, len(base["scenes"])))
        text = m.group(2).strip().strip("\"'“”") or "新场景"
        base["scenes"].insert(pos, {
            "type": "text", "text": text, "duration": 3,
            "bg": "#16213e", "animation": "fade_in",
        })
        return out(f"已在第 {pos+1} 位插入场景「{text}」（共 "
                   f"{len(base['scenes'])} 个场景）", base)

    # 8e. 模板场景插入：模板名来自 templates/ 目录（热加载）
    m = re.search(r"加一?个?\s*([\w\-]+)\s*(?:模板|template)\s*场景", msg, re.I)
    if m:
        import hf_compose
        registry = hf_compose.load_custom_templates(spec_dir)
        tname = m.group(1)
        if tname not in registry:
            names = "、".join(sorted(registry))
            return out(f"模板「{tname}」不存在。可用模板：{names or '（无）'}",
                       mode="help")
        defn = registry[tname]
        scene = {"type": tname, "duration": 3}
        for slot in defn.get("slots", []):
            scene[slot] = f"[{slot}]"
        base.setdefault("scenes", []).append(scene)
        return out(f"已追加「{tname}」模板场景（占位 slot："
                   f"{'、'.join(defn.get('slots', [])) or '无'}——分镜/Inspector "
                   "里把 [slot] 换成实际内容）", base)

    # 8f. BGM 音量 / 音轨音量（audio 段全局）
    m = re.search(r"(?:背景音乐|BGM|bgm).{0,6}音量\s*([\d.]+)", msg, re.I)
    if m:
        vol = max(0.0, min(1.0, float(m.group(1))))
        audio = base.setdefault("audio", {})
        bgm = audio.get("bgm") if isinstance(audio.get("bgm"), dict) else {}
        bgm["volume"] = vol
        audio["bgm"] = bgm
        return out(f"BGM 音量已设为 {vol}", base)

    # 8g. 扩展高频本地编辑：单镜复制 / 移动 / 交换 / 加减时长 / 全局节奏 / 统一转场动画 / 内置卡片
    if re.search(r"(?:复制|拷贝|duplicate).{0,6}第\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|镜)", msg, re.I) or (
            ctx_idx is not None and re.search(r"(?:复制|拷贝).{0,4}(?:这|当前)", raw_msg)):
        m_dup = re.search(r"第\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|镜)", msg)
        idx = (_int(m_dup.group(1), 0) - 1) if m_dup else ctx_idx
        if idx is not None and 0 <= idx < len(base["scenes"]):
            dup = copy.deepcopy(base["scenes"][idx])
            base["scenes"].insert(idx + 1, dup)
            return out(f"已复制第 {idx + 1} 镜并插到第 {idx + 2} 位", base)

    m_swap = re.search(
        r"(?:交换|互换|对调)\s*第?\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|镜)?\s*(?:和|与|跟)\s*第?\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|镜)?",
        msg)
    if m_swap:
        a, b = _int(m_swap.group(1), 0) - 1, _int(m_swap.group(2), 0) - 1
        n_sc = len(base["scenes"])
        if 0 <= a < n_sc and 0 <= b < n_sc and a != b:
            base["scenes"][a], base["scenes"][b] = base["scenes"][b], base["scenes"][a]
            return out(f"已交换第 {a + 1} 镜与第 {b + 1} 镜的顺序", base)

    m_mv = re.search(
        r"(?:把\s*)?第\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|镜)\s*(?:向|往)?(前|上|后|下)\s*(?:移动|挪|移)",
        msg)
    if m_mv:
        idx = _int(m_mv.group(1), 0) - 1
        delta = -1 if m_mv.group(2) in ("前", "上") else 1
        nxt = idx + delta
        if 0 <= idx < len(base["scenes"]) and 0 <= nxt < len(base["scenes"]):
            sc = base["scenes"].pop(idx)
            base["scenes"].insert(nxt, sc)
            return out(f"已将第 {idx + 1} 镜移至第 {nxt + 1} 位", base)

    m_delta = re.search(
        r"第\s*([一二三四五六七八九十\d]+)\s*个?\s*(?:场景|镜).{0,4}(加长|延长|增加|缩短|减少|减短)\s*([\d.]+)\s*秒",
        msg)
    if m_delta:
        idx = _int(m_delta.group(1), 0) - 1
        delta = float(m_delta.group(3))
        if m_delta.group(2) in ("缩短", "减少", "减短"):
            delta = -delta
        if 0 <= idx < len(base["scenes"]):
            cur_d = float(base["scenes"][idx].get("duration", 3))
            new_d = round(max(0.5, min(3600.0, cur_d + delta)), 2)
            base["scenes"][idx]["duration"] = new_d
            return out(f"第 {idx + 1} 镜时长已调整为 {new_d}s", base)

    if re.search(r"^(?:整体|全片)?(?:加快节奏|节奏快一点|紧凑一点|提速|太慢了)$", raw_msg):
        for sc in base["scenes"]:
            cur_d = float(sc.get("duration", 3))
            sc["duration"] = round(max(1.2, cur_d * 0.8), 2)
        return out("已整体提速，所有场景时长压缩至约 80%", base)

    if re.search(r"^(?:整体|全片)?(?:放慢节奏|节奏慢一点|留出呼吸感|舒缓一点|太快了)$", raw_msg):
        for sc in base["scenes"]:
            cur_d = float(sc.get("duration", 3))
            sc["duration"] = round(min(15.0, cur_d * 1.25), 2)
        return out("已整体放慢节奏，各镜时长增加约 25%", base)

    m_all_tr = re.search(r"(?:全部|所有场景|统一|全局)\s*转场\s*(?:改成|改为|设为)?\s*[:：]?\s*(\S+)", msg, re.I)
    if m_all_tr:
        val = m_all_tr.group(1).strip().lower()
        if val in ("fade", "slide-left", "slide-right", "wipe", "zoom", "blur", "none"):
            for i, sc in enumerate(base["scenes"]):
                if i > 0:
                    sc["transition"] = val
            base["transitions"] = {"type": val, "duration": 0.4}
            return out(f"已将全片转场统一设置为 {val}", base)

    # 8h. 场景类型转换（把第 N 镜改成统计卡 / 对比卡 / 金句 / 清单 / 标题）
    m_conv = re.search(
        r"第?\s*([一二三四五六七八九十\d]+)\s*个?场景.{0,6}(?:改成|改为|变成|换成|转为)\s*(标题|title|统计|数据|stats|对比|comparison|金句|引用|quote|列表|清单|list|正文|text|片尾|outro)",
        msg, re.I)
    if m_conv:
        idx = _int(m_conv.group(1), 0) - 1
        tmap = {"标题": "title", "title": "title",
                "统计": "stats", "数据": "stats", "stats": "stats",
                "对比": "comparison", "comparison": "comparison",
                "金句": "quote", "引用": "quote", "quote": "quote",
                "列表": "list", "清单": "list", "list": "list",
                "正文": "text", "text": "text",
                "片尾": "outro", "outro": "outro"}
        nt = tmap.get(m_conv.group(2).lower())
        if nt and 0 <= idx < len(base["scenes"]):
            sc = base["scenes"][idx]
            sc["type"] = nt
            if nt == "stats":
                sc.setdefault("value", "10×")
                sc.setdefault("description", sc.get("text") or "关键指标提升")
            elif nt == "comparison":
                sc.setdefault("text", sc.get("text") or "方案对比")
                sc.setdefault("left", "改进前")
                sc.setdefault("right", "改进后")
                sc.setdefault("left_items", ["手工操作", "耗时较长"])
                sc.setdefault("right_items", ["一键自动", "稳定可控"])
            elif nt == "quote":
                sc.setdefault("quote", sc.get("text") or "少即是多，让内容自己说话。")
                sc.setdefault("author", "产品团队")
            elif nt == "list":
                sc.setdefault("text", sc.get("text") or "核心要点")
                sc.setdefault("items", ["要点一", "要点二", "要点三"])
            return out(f"第 {idx + 1} 个场景已切换为「{nt}」类型", base)

    # 8i. 视觉预设切换（切换全局 design preset）
    if re.search(r"风格|预设|preset|配色|皮肤|主题|太暗|亮色|暖石|霓虹|森|珊瑚|海报|暖纸", msg, re.I):
        pname = _match_preset_name(msg)
        if pname and not re.search(r"生成|制作|做一?个|做一?条|新建|创建", msg):
            pobj = _apply_design_preset(base, pname)
            if pobj:
                desc = (pobj.get("description") or "").split("：")[0]
                return out(
                    f"已将全片视觉切换为「{pname}」（{desc}）",
                    base,
                    suggestions=["全部转场改成 wipe", "节奏慢一点", "换成 editorial-stone 风格" if pname != "editorial-stone" else "换成 midnight-glow 风格"],
                )

    # 8j. 零配置自然语言 0-to-1 生成整条视频
    if re.search(r"(?:帮我)?(?:生成|制作|做|写|创建)(?:一个|一条|份)?.*?(?:视频|短片|宣传片|介绍片|演示)", msg, re.I) \
            and not re.search(r"模板|片尾|launch|四段", msg, re.I):
        pname = _match_preset_name(msg) or "midnight-glow"
        fmt = ("9:16" if re.search(r"竖屏|9:16|抖音|小红书|视频号", msg, re.I)
               else "1:1" if re.search(r"1:1|方形", msg) else "16:9")
        arc = ("comparison" if re.search(r"对比|vs|pk", msg, re.I)
               else "listicle" if re.search(r"盘点|亮点|清单|步骤", msg)
               else "launch")
        topic_m = re.search(
            r"(?:关于|介绍|宣传|讲解|做(?:一个|一条)?)\s*([^，,。；;！？\n]{2,36}?)(?:的)?(?:15秒|30秒)?(?:产品|品牌)?(?:宣传片|介绍片|短视频|演示视频|短片|视频|介绍)",
            msg, re.I)
        topic = topic_m.group(1).strip() if topic_m else re.sub(
            r"(?:帮我|请|生成|制作|创建|一个|一条|关于|视频|短片|宣传片|采用|风格|竖屏|横屏|16:9|9:16|"
            + r"midnight-glow|editorial-stone|blue-professional|editorial-forest|coral-pop|code-editorial|bold-poster)",
            " ", msg, flags=re.I).strip(" ：:，,。")
        if len(topic) < 2:
            topic = "智能效率工作流"
        gen_spec = _build_zero_config_video(topic, fmt=fmt, preset_name=pname, arc=arc)
        total_dur = sum(float(s.get("duration", 3)) for s in gen_spec["scenes"])
        return out(
            f"已为你生成「{topic}」{len(gen_spec['scenes'])} 镜成片"
            f"（{fmt} · {pname} 视觉预设，约 {total_dur:.1f}s）。你可以继续点选下方建议或选中任意一镜微调。",
            gen_spec,
            suggestions=["换成 editorial-stone 风格", "全部转场改成 wipe", "改成竖屏 9:16", "生成旁白"],
        )

    # 8k. Framey 式宽泛意图方向卡片（选项先于开放式追问）
    has_llm_early = bool((llm_config or {}).get("endpoint") or
                         os.environ.get("XMEDIA_LLM_ENDPOINT", "").strip())
    if not has_llm_early and re.search(
            r"^(?:帮我)?(?:想|策划|构思|怎么拍).{0,8}(?:视频|短片)?|"
            r"^(?:我想做|打算做|要做)(?:一个|个)?(?:产品)?(?:视频|短片|宣传片)$|"
            r"(?:有什么|给点).{0,4}(?:创意|灵感|方向)|"
            r"(?:太单调|没质感|不够高级|怎么改|如何优化)", msg, re.I):
        cur_p = base.get("design") or "default"
        return out(
            f"我整理了 3 个可直接落地的视觉与结构方向（当前预设 `{cur_p}`），点选一张卡片即可立即应用：",
            mode="guide",
            choices=[
                {"label": "🚀 暗夜霓虹科技风（midnight-glow）",
                 "desc": "深靛夜空 + 冰蓝发光标牌，适合 SaaS/AI 工具发布片",
                 "msg": "切换成 midnight-glow 风格"},
                {"label": "📜 暖石极简画报风（editorial-stone）",
                 "desc": "米暖底色 + 衬线大字 + 慷慨留白，质感沉稳高级",
                 "msg": "切换成 editorial-stone 风格"},
                {"label": "⚡ 珊瑚高对比冲击风（coral-pop）",
                 "desc": "高反差珊瑚红强调 + 紧实节奏，适合社交传播短片",
                 "msg": "切换成 coral-pop 风格"},
            ],
            suggestions=["把第 1 个场景改成 stats", "全部转场改成 wipe", "帮我做一个 AI 助手介绍视频"],
        )

    m_card = re.search(
        r"(?:加|追加|新增)一?个?\s*(标题|title|统计|数据|stats|引用|金句|quote|对比|comparison|列表|清单|list)\s*(?:场景|卡|页)?(?:[:：]\s*(.+))?$",
        raw_msg, re.I)
    if m_card:
        kind_raw = m_card.group(1).lower()
        txt = (m_card.group(2) or "").strip().strip("\"'“”")
        card_map = {
            "标题": ("title", {"type": "title", "text": txt or "核心主题标题",
                               "subtitle": "一句话说明核心价值", "duration": 3,
                               "bg": "#1a1a2e"}),
            "title": ("title", {"type": "title", "text": txt or "核心主题标题",
                                "subtitle": "一句话说明核心价值", "duration": 3,
                                "bg": "#1a1a2e"}),
            "统计": ("stats", {"type": "stats", "value": txt or "99%",
                               "description": "关键指标对比提升", "duration": 3,
                               "bg": "#16213e"}),
            "数据": ("stats", {"type": "stats", "value": txt or "99%",
                               "description": "关键指标对比提升", "duration": 3,
                               "bg": "#16213e"}),
            "stats": ("stats", {"type": "stats", "value": txt or "99%",
                                "description": "关键指标对比提升", "duration": 3,
                                "bg": "#16213e"}),
            "引用": ("quote", {"type": "quote", "quote": txt or "少即是多，让画面自己说话。",
                               "author": "— 设计笔记", "duration": 3.5,
                               "bg": "#111827"}),
            "金句": ("quote", {"type": "quote", "quote": txt or "少即是多，让画面自己说话。",
                               "author": "— 设计笔记", "duration": 3.5,
                               "bg": "#111827"}),
            "quote": ("quote", {"type": "quote", "quote": txt or "少即是多，让画面自己说话。",
                                "author": "— 设计笔记", "duration": 3.5,
                                "bg": "#111827"}),
            "对比": ("comparison", {"type": "comparison", "left": "传统流程",
                                    "right": "确定性新方案",
                                    "left_items": ["耗时手工调整", "结果不稳定"],
                                    "right_items": ["一句话驱动生成", "逐帧可复现"],
                                    "duration": 4, "bg": "#16213e"}),
            "comparison": ("comparison", {"type": "comparison", "left": "传统流程",
                                          "right": "确定性新方案",
                                          "left_items": ["耗时手工调整", "结果不稳定"],
                                          "right_items": ["一句话驱动生成", "逐帧可复现"],
                                          "duration": 4, "bg": "#16213e"}),
            "列表": ("list", {"type": "list", "text": txt or "三大核心亮点",
                              "items": ["要点一", "要点二", "要点三"],
                              "duration": 3.5, "bg": "#1a1a2e"}),
            "清单": ("list", {"type": "list", "text": txt or "三大核心亮点",
                              "items": ["要点一", "要点二", "要点三"],
                              "duration": 3.5, "bg": "#1a1a2e"}),
            "list": ("list", {"type": "list", "text": txt or "三大核心亮点",
                              "items": ["要点一", "要点二", "要点三"],
                              "duration": 3.5, "bg": "#1a1a2e"}),
        }
        if kind_raw in card_map:
            tname, card_obj = card_map[kind_raw]
            base["scenes"].append(card_obj)
            return out(f"已追加「{tname}」场景卡片（共 {len(base['scenes'])} 镜）", base)

    # 9. 管线动作
    if re.search(r"生成旁白|配音|tts|朗读", msg, re.I):
        return out("开始生成全场景旁白 …", action="tts")
    if re.search(r"自动字幕|语音转写|ASR|识别字幕", msg, re.I):
        return out("开始识别素材语音并生成分段字幕 …", action="asr")
    if re.search(r"烘焙|导出|render|bake|出片", msg, re.I):
        return out("开始导出 MP4 …", action="bake")
    if re.search(r"重组合成|刷新预览|recompose", msg, re.I):
        return out("已重组合成", action="recompose")

    # 10. 可选 LLM 兜底（环境变量或 Studio 运行时配置的 OpenAI 兼容 API）
    has_llm = bool((llm_config or {}).get("endpoint") or
                   os.environ.get("XMEDIA_LLM_ENDPOINT", "").strip())
    if has_llm:
        try:
            import hf_compose
            tpl_names = sorted(hf_compose.load_custom_templates(
                spec_dir or ".").keys())
        except Exception:
            tpl_names = []
        try:
            data = _llm_complete(
                raw_msg, base, context=context, history=history,
                templates=tpl_names, spec_dir=spec_dir, llm_config=llm_config)
            sugs = data.get("suggestions") if isinstance(
                data.get("suggestions"), list) else None
            chs = data.get("choices") if isinstance(
                data.get("choices"), list) else None
            if "spec" in data and data["spec"] is not None:
                candidate = data["spec"]
                validate_spec(candidate, spec_dir)
                return out(str(data.get("reply") or "已按指令更新 spec"),
                           candidate, mode="llm", suggestions=sugs, choices=chs)
            return out(str(data.get("reply") or "已收到"),
                       None, mode="llm", suggestions=sugs, choices=chs)
        except (LlmUnavailable, AgentRejected) as e:
            return out(f"LLM 未能应用（{e}）。{HELP}", mode="help")
    return out("我不确定这条指令要改什么。" + HELP, mode="help",
               suggestions=["加一个标题卡", "全部转场改成 fade", "整体加快节奏"])


def main():
    p = argparse.ArgumentParser(description="xmedia-kit Agent 指令自检")
    p.add_argument("message")
    p.add_argument("spec", nargs="?", default="")
    p.add_argument("--spec-dir", default="")
    args = p.parse_args()
    spec = {}
    if args.spec:
        with open(args.spec, "r", encoding="utf-8") as f:
            spec = json.load(f)
    result = handle(args.message, spec, args.spec_dir)
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
