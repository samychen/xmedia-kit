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
SYSTEM_PROMPT = """你是 xmedia-kit 的视频编辑 Agent。用户给你一句自然语言指令和当前 spec JSON。
只输出一个 JSON 对象（不要 markdown 代码块、不要解释文字）：
{"reply": "一句中文说明你做了什么", "spec": {完整的新 spec JSON}}
spec 规则：顶层 schema_version 固定 "1.0"；composition 含 format(16:9/9:16/1:1/4:5) 与 fps；
scenes 是非空数组，每项含 type(title/text/outro/quote/list/stats/comparison 或自定义模板名)、
text、duration(秒)、bg(十六进制色)、可选 animation、video(本地素材路径)、
narration(旁白音频路径)、captions(场景相对字幕段 [{start,end,text}])；
顶层可选 captions:{enabled,segments:[{start,end,text}]}(全局时间轴绝对秒)、
audio:{src,volume,fade_in,fade_out}、sfx:{enabled}、transitions:{type,duration}。
不要写 HTML、不要写 JavaScript、不要编造不存在的素材路径。"""


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


def _llm_complete(message, spec, timeout=60):
    endpoint = os.environ.get("XMEDIA_LLM_ENDPOINT", "").strip()
    if not endpoint:
        raise LlmUnavailable("未配置 XMEDIA_LLM_ENDPOINT")
    key = os.environ.get("XMEDIA_LLM_API_KEY", "")
    model = os.environ.get("XMEDIA_LLM_MODEL", "gpt-4o-mini")
    payload = {"model": model, "temperature": 0, "messages": [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": json.dumps(
            {"instruction": message, "current_spec": spec},
            ensure_ascii=False)}]}
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
    if not isinstance(data, dict) or "spec" not in data:
        raise LlmUnavailable("LLM 响应缺少 spec 字段")
    return data


HELP = ("可用指令：导入 <目录> / 加片尾卡 / 删除第 N 个场景 / "
        "第 N 个场景文字改成 X / 改成竖屏 / 背景改成 #1a1a2e / "
        "关闭字幕 / 生成旁白 / 自动字幕(ASR) / 导出 MP4 / 第 N 个场景时长 X 秒。"
        " 配置 XMEDIA_LLM_ENDPOINT 后可直接用自由自然语言。")


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


def _int(s, default=0):
    cn = {"一": 1, "二": 2, "三": 3, "四": 4, "五": 5,
          "六": 6, "七": 7, "八": 8, "九": 9, "十": 10}
    s = (s or "").strip()
    if s.isdigit():
        return int(s)
    return cn.get(s, default)


def handle(message, spec, spec_dir=""):
    """→ {reply, spec?, action?, args?, mode}。spec 为 None 表示不改 spec。"""
    msg = str(message or "").strip()
    base = copy.deepcopy(spec) if isinstance(spec, dict) else {}
    if not base.get("scenes"):
        base = {"schema_version": "1.0",
                "composition": {"id": "main", "format": "16:9", "fps": 30},
                "scenes": [{"type": "text", "text": "", "duration": 3}]}

    def out(reply, new_spec=None, action=None, args=None, mode="rule"):
        payload = {"reply": reply, "mode": mode}
        if new_spec is not None:
            new_spec = copy.deepcopy(new_spec)
            new_spec["schema_version"] = "1.0"
            payload["spec"] = new_spec
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
        if brand.get("voice"):
            base.setdefault("tts", {})["voice"] = brand["voice"]
        for sc in base.get("scenes", []):
            if sc.get("type") == "outro":
                if brand.get("outro_text"):
                    sc["text"] = brand["outro_text"]
                if brand.get("logo_text") and "subtitle" in sc:
                    sc["subtitle"] = brand["logo_text"]
        return out(f"品牌「{m.group(1)}」已应用（色板 {len(palette)} 色、"
                   f"文字 {text or '不变'}、音色 "
                   f"{brand.get('voice') or '不变'}）", base)

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

    # 9. 管线动作
    if re.search(r"生成旁白|配音|tts|朗读", msg, re.I):
        return out("开始生成全场景旁白 …", action="tts")
    if re.search(r"自动字幕|语音转写|ASR|识别字幕", msg, re.I):
        return out("开始识别素材语音并生成分段字幕 …", action="asr")
    if re.search(r"烘焙|导出|render|bake|出片", msg, re.I):
        return out("开始导出 MP4 …", action="bake")
    if re.search(r"重组合成|刷新预览|recompose", msg, re.I):
        return out("已重组合成", action="recompose")

    # 10. 可选 LLM 兜底
    if os.environ.get("XMEDIA_LLM_ENDPOINT", "").strip():
        try:
            data = _llm_complete(msg, spec)
            candidate = data["spec"]
            validate_spec(candidate, spec_dir)
            return out(str(data.get("reply") or "已按指令更新 spec"),
                       candidate, mode="llm")
        except (LlmUnavailable, AgentRejected) as e:
            return out(f"LLM 未能应用（{e}）。{HELP}", mode="help")
    return out("我不确定这条指令要改什么。" + HELP, mode="help")


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
