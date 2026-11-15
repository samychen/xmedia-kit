#!/usr/bin/env python3
# xmedia CLI：AI 视频生成管线的统一入口（生态门面）。
#
# 子命令全部委托既有工具（hf_tts/hf_compose/hf_validate/hf_studio）与
# html_bake 二进制——本文件只做编排、参数推导与 doctor 环境自检。
#
#   xmedia render <spec.json> --out out.mp4   # 全链：TTS→compose→validate→bake
#   xmedia tts/compose/validate/studio ...    # 单步（透传既有工具参数）
#   xmedia init <dir>                          # 脚手架：可运行的示例 spec
#   xmedia sfx                                 # 再生成 assets/sfx 音效
#   xmedia doctor                              # 环境自检（表格式报告）

import argparse
import html as _html
import json
import os
import re
import shutil
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TOOLS_DIR)
FORMAT_PRESETS = {"16:9": (1920, 1080), "9:16": (1080, 1920),
                  "1:1": (1080, 1080), "4:5": (1080, 1350)}


def root(*parts):
    return os.path.join(ROOT_DIR, *parts)


def bake_bin():
    """html_bake 可执行文件：macOS bundle 优先，Linux 裸二进制兜底。"""
    cands = [root("build", "bin", "html_bake.app", "Contents", "MacOS",
                  "html_bake"),
             root("build", "bin", "html_bake")]
    for c in cands:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None


def py_tool(name):
    return os.path.join(TOOLS_DIR, name)


def run(cmd, **kw):
    print("+ " + " ".join(shlex_quote(c) for c in cmd), file=sys.stderr)
    proc = subprocess.run(cmd, **kw)
    if proc.returncode != 0:
        sys.exit(proc.returncode)
    return proc


def shlex_quote(c):
    return "'" + c.replace("'", "'\\''") + "'" if (" " in c or "'" in c) else c


def load_spec(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def spec_dims(spec):
    comp = spec.get("composition", {})
    fmt = comp.get("format")
    if fmt in FORMAT_PRESETS:
        return FORMAT_PRESETS[fmt]
    return (int(comp.get("width", 1080)), int(comp.get("height", 1920)))


def cmd_render(args):
    """全链：TTS（可选）→ compose → validate → bake。任何一步失败即止。"""
    spec = load_spec(args.spec)
    w, h = spec_dims(spec)
    fps = int(spec.get("composition", {}).get("fps", 30))
    out_dir = os.path.dirname(os.path.abspath(args.spec))
    stem = os.path.splitext(os.path.basename(args.spec))[0]
    html = os.path.join(out_dir, stem + ".html")

    if not args.no_tts:
        run([sys.executable, py_tool("hf_tts.py"), args.spec,
             "-o", os.path.join(out_dir, stem + ".narrated.json")])
        spec = load_spec(os.path.join(out_dir, stem + ".narrated.json"))
        args.spec = os.path.join(out_dir, stem + ".narrated.json")

    run([sys.executable, py_tool("hf_compose.py"), args.spec, "-o", html])
    run([sys.executable, py_tool("hf_validate.py"), html])

    # 总时长从契约页读取（compose 拟合后的权威值）。
    html_text = open(html, encoding="utf-8").read()
    m = re.search(r'data-composition-id[^>]*data-duration="([\d.]+)"',
                  html_text, re.S)
    if not m:
        print("xmedia: 契约页缺少 data-duration——compose 产物异常", file=sys.stderr)
        sys.exit(1)
    seconds = float(m.group(1))
    bake = bake_bin()
    if not bake:
        print("xmedia: 找不到 html_bake——先构建（build/bin/html_bake.app）",
              file=sys.stderr)
        sys.exit(1)
    cmd = [bake, "--url", "file://" + html, "--out", args.out,
           "--seconds", f"{seconds:.2f}", "--fps", str(fps),
           "--width", str(w), "--height", str(h), "--clock", "seek"]
    if args.chunks > 1:
        cmd += ["--chunks", str(args.chunks)]
    run(cmd)
    print(f"xmedia: 完成 {args.out}（{seconds:.1f}s / {w}x{h} @ {fps}fps）")


def md_inline(s):
    """行内元素：**粗体**、`代码`、[文本](链接)。"""
    s = re.sub(r"\*\*([^*]+)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)",
               r'<a href="\2">\1</a>', s)
    return s


def md_to_html(md):
    """HF_COMPOSE.md 子集的最小转换器（标题/代码块/表格/列表/引用/
    分隔线/段落）——零第三方依赖，`xmedia docs` 一条命令保持文档站
    与真源同步。"""
    out = []
    in_code = False
    code_lang = ""
    code_buf = []
    table_buf = []
    in_list = False
    for line in md.splitlines():
        if line.strip().startswith("```"):
            if in_code:
                out.append(f'<pre><code class="{code_lang}">'
                           + "\n".join(code_buf) + "</code></pre>")
                code_buf = []
                in_code = False
            else:
                # 悬空缓冲（表格/列表）先收口
                if table_buf:
                    out.append("<table>" + "".join(table_buf) + "</table>")
                    table_buf = []
                if in_list:
                    out.append("</ul>")
                    in_list = False
                in_code = True
                code_lang = line.strip()[3:].strip()
            continue
        if in_code:
            code_buf.append(line.replace("&", "&amp;")
                            .replace("<", "&lt;").replace(">", "&gt;"))
            continue
        stripped = line.strip()
        if stripped.startswith("|") and stripped.endswith("|"):
            cells = [c.strip() for c in stripped.strip("|").split("|")]
            if all(re.fullmatch(r"-{3,}:?|:?-{3,}:?", c) for c in cells):
                continue  # 分隔行
            tag = "th" if not table_buf else "td"
            table_buf.append("<tr>" + "".join(
                f"<{tag}>{md_inline(c)}</{tag}>" for c in cells) + "</tr>")
            continue
        if table_buf:
            out.append("<table>" + "".join(table_buf) + "</table>")
            table_buf = []
        m = re.match(r"^(#{1,4})\s+(.*)", line)
        if m:
            level = len(m.group(1))
            anchor = re.sub(r"[^\w\u4e00-\u9fff]+", "-",
                            m.group(2)).strip("-").lower()
            out.append(f"<h{level} id='{anchor}'>{md_inline(m.group(2))}"
                       f"</h{level}>")
            continue
        if stripped.startswith("- "):
            if not in_list:
                out.append("<ul>")
                in_list = True
            out.append(f"<li>{md_inline(stripped[2:])}</li>")
            continue
        if in_list and stripped:
            out.append(f"<li>{md_inline(stripped)}</li>")
            continue
        if in_list:
            out.append("</ul>")
            in_list = False
        if stripped == "---":
            out.append("<hr>")
            continue
        if stripped:
            out.append(f"<p>{md_inline(stripped)}</p>")
    if table_buf:
        out.append("<table>" + "".join(table_buf) + "</table>")
    if in_list:
        out.append("</ul>")
    return "\n".join(out)


# 文档站落地页：作品展示文案（其余字段一律从 spec / ffprobe 实测，
# 避免 README 式数字漂移）。键名对应 gallery/<键>.json 与 .mp4。
WORKS_INFO = {
    "product-launch": (
        "产品发布会",
        "16:9 横屏：自定义 lower-third 人名条、stats 场景与 wipe / fade 转场。",
        ("16:9", "自定义模板", "转场")),
    "knowledge-cards": (
        "竖屏知识卡片",
        "9:16 竖屏：list / stats 场景、slide 转场与内嵌字幕。",
        ("9:16", "字幕", "list")),
    "event-invite": (
        "活动邀请函",
        "9:16 竖屏：wipe 转场、quote 场景与 BGM 确定性混音。",
        ("9:16", "BGM", "quote")),
    "demo": (
        "管线快速示例",
        "官方 CLI 示例：标题 + 数据场景、TTS 旁白与烘焙 MP4。",
        ("9:16", "TTS", "入门")),
}

# Hero 三段介绍（对位 Remotion 官网 hero 下的三栏切换轮播）。
INTRO_SLIDES = (
    ("product-launch", "写一份 spec",
     "画幅、场景、动画与时序都声明在 JSON 里；改一个字段就是一条新片。"),
    ("knowledge-cards", "帧级预览",
     "Studio 等比缩放画布、逐帧步进，播放头与场景 / 属性双向联动。"),
    ("event-invite", "确定性烘焙",
     "CEF 离屏 → GL 合成 → FFmpeg 编码，同 spec 两次输出逐字节一致。"),
)


def ffprobe_meta(path):
    """实测视频宽高与时长；ffprobe 不可用或失败时返回空字典。"""
    probe = shutil.which("ffprobe")
    if not probe:
        return {}
    try:
        proc = subprocess.run(
            [probe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "stream=width,height:format=duration",
             "-of", "json", path],
            capture_output=True, text=True, timeout=20)
        info = json.loads(proc.stdout)
        stream = (info.get("streams") or [{}])[0]
        w, h = stream.get("width"), stream.get("height")
        dur = float((info.get("format") or {}).get("duration") or 0)
        return {"dims": f"{w} × {h}" if w and h else "",
                "ratio": "16:9" if w and h and w > h else "9:16",
                "dur": dur}
    except (OSError, ValueError, subprocess.SubprocessError):
        return {}


def collect_works():
    """从仓库内的真实 spec + MP4 收集作品，缺任一方即跳过。"""
    works = []
    for wid, (title, desc, tags) in WORKS_INFO.items():
        base = root("demo-output" if wid == "demo" else "gallery")
        spec = os.path.join(base, wid + ".json")
        mp4 = os.path.join(base, wid + ".mp4")
        if not (os.path.isfile(spec) and os.path.isfile(mp4)):
            continue
        try:
            with open(spec, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        comp = data.get("composition", {})
        scenes = data.get("scenes", [])
        meta = ffprobe_meta(mp4)
        if not meta.get("dims"):
            fmt = comp.get("format", "9:16")
            w, h = FORMAT_PRESETS.get(fmt, (1080, 1920))
            w = comp.get("width", w)
            h = comp.get("height", h)
            meta.update(dims=f"{w} × {h}",
                        ratio="16:9" if w > h else "9:16", dur=0)
        # 舞台右侧代码面板展示真实 spec 节选，而不是手工编的假代码。
        sample = {"composition": comp, "scenes": scenes[:1]}
        works.append({
            "id": wid, "title": title, "desc": desc, "tags": tags,
            "file": wid + ".json", "spec": spec, "mp4": mp4,
            "href": os.path.join("..", os.path.basename(base), wid + ".json")
                    .replace(os.sep, "/"),
            "contract": os.path.join("..", os.path.basename(base),
                                     wid + ".html").replace(os.sep, "/"),
            "dims": meta["dims"], "ratio": meta["ratio"],
            "dur": meta["dur"], "fps": comp.get("fps", 30),
            "scenes": len(scenes),
            "code": json.dumps(sample, ensure_ascii=False, indent=2),
            "video": "", "poster": "",
        })
    return works


def ensure_previews(works):
    """把 gallery 的 MJPEG MP4 转成网页可播的 H.264 预览 + 封面帧。

    浏览器不保证支持 MP4 容器里的 mjpeg，所以展示层不能直接引用烘焙产物。
    结果缓存于 docs-site/media/（源更新则重建）；无 ffmpeg 时静默降级，
    页面自动回退到占位封面，文档生成不受影响。
    """
    ffmpeg = shutil.which("ffmpeg")
    media = root("docs-site", "media")
    os.makedirs(media, exist_ok=True)
    for w in works:
        src = w["mp4"]
        preview = os.path.join(media, w["id"] + ".mp4")
        cover = os.path.join(media, w["id"] + ".jpg")
        if not ffmpeg or not os.path.isfile(src):
            continue

        def stale(path):
            return (not os.path.isfile(path)) or \
                   os.path.getmtime(path) < os.path.getmtime(src)

        if stale(preview):
            try:
                subprocess.run(
                    [ffmpeg, "-y", "-loglevel", "error", "-i", src,
                     "-t", "6", "-vf", "scale=-2:720", "-c:v", "libx264",
                     "-crf", "30", "-preset", "veryfast", "-pix_fmt", "yuv420p",
                     "-an", "-movflags", "+faststart", preview],
                    capture_output=True, timeout=120, check=False)
            except (OSError, subprocess.SubprocessError):
                pass
        if stale(cover):
            try:
                subprocess.run(
                    [ffmpeg, "-y", "-loglevel", "error", "-ss", "1.2",
                     "-i", src, "-frames:v", "1", "-vf", "scale=-2:720",
                     "-q:v", "6", cover],
                    capture_output=True, timeout=60, check=False)
            except (OSError, subprocess.SubprocessError):
                pass
        rel = "media/"
        if os.path.isfile(preview):
            w["video"] = rel + w["id"] + ".mp4"
        if os.path.isfile(cover):
            w["poster"] = rel + w["id"] + ".jpg"


def render_works_html(works):
    """作品轮播卡片：视频懒加载，缺失素材以渐变封面兜底。"""
    if not works:
        return ('<article class="work-card"><div class="work-media">'
                '<div class="ph" style="display:block"></div></div>'
                '<div class="work-info"><h3>暂无可展示作品</h3>'
                '<p>运行 xmedia render 生成 spec / MP4 后，再次执行 '
                'xmedia docs 即会出现在这里。</p></div></article>')
    out = []
    for w in works:
        esc = _html.escape
        tags = "".join(f"<span>{esc(t)}</span>" for t in w["tags"])
        video = (f'<video muted loop playsinline preload="none" '
                 f'data-src="{w["video"]}" poster="{w["poster"]}"></video>'
                 if w["video"] else "")
        ph = "" if w["video"] else ' style="display:block"'
        dur = f'{w["dur"]:.1f}s' if w["dur"] else "—"
        wide = " wide" if w["ratio"] == "16:9" else ""
        out.append(f'''<article class="work-card{wide}">
  <div class="work-media">{video}<div class="ph"{ph}></div>
    <span class="tag">{w["ratio"]} · {dur}</span>
    <span class="play" aria-hidden="true"></span></div>
  <div class="work-info"><h3>{esc(w["title"])}</h3>
    <p>{esc(w["desc"])}</p><div class="tags">{tags}</div>
    <div class="work-links"><a href="{w["href"]}" target="_blank">spec ↗</a>
      <a href="{w["contract"]}" target="_blank">契约页 ↗</a>
      <a href="{w["video"] or w["poster"]}" target="_blank">预览 ↗</a></div>
  </div></article>''')
    return "\n".join(out)


def build_landing(body):
    """把 markdown 正文注入落地页模板：轮播数据、卡片、统计与目录。"""
    works = collect_works()
    ensure_previews(works)
    by_id = {w["id"]: w for w in works}

    slides = []
    fallback_code = ('{\n  "composition": {"format": "9:16", "fps": 30},\n'
                     '  "scenes": []\n}')
    for i, (wid, title, desc) in enumerate(INTRO_SLIDES, 1):
        w = by_id.get(wid, {})
        slides.append({
            "no": f"{i:02d}", "title": title, "desc": desc,
            "file": w.get("file", wid + ".json"),
            "dims": w.get("dims", "1080 × 1920"),
            "fps": w.get("fps", 30),
            "code": w.get("code", fallback_code),
            "video": w.get("video", ""), "poster": w.get("poster", ""),
        })

    templates = len([f for f in os.listdir(root("templates"))
                     if f.endswith(".json")]) if os.path.isdir(root("templates")) else 0
    stats = (
        (str(len(works)), "REPRODUCIBLE WORKS"),
        (str(sum(w["scenes"] for w in works)), "SCENES IN SPECS"),
        (str(templates), "CUSTOM TEMPLATES"),
        ("3×", "PROVENANCE ANCHORS"),
    )
    stats_html = "".join(
        f'<div class="stat"><b>{v}</b><span>{k}</span></div>' for v, k in stats)
    toc = "".join(
        f'<a href="#{m.group(1)}">{m.group(2)}</a>'
        for m in re.finditer(r"<h2 id='([^']+)'>(.*?)</h2>", body))

    with open(root("tools", "docs_landing.html"), "r", encoding="utf-8") as f:
        tmpl = f.read()
    # JSON 内联进 <script>，转义 </ 以免提前闭合脚本标签。
    data = json.dumps(slides, ensure_ascii=False).replace("</", "<\\/")
    for key, val in (("@@SLIDES@@", data),
                     ("@@WORKS@@", render_works_html(works)),
                     ("@@STATS@@", stats_html),
                     ("@@TOC@@", toc),
                     ("@@BODY@@", body)):
        tmpl = tmpl.replace(key, val)
    return tmpl


def cmd_docs(args):
    """文档站：docs/HF_COMPOSE.md → docs-site/index.html。

    落地页模板在 tools/docs_landing.html；作品预览由 gallery 实拍产物
    转码到 docs-site/media/。正文仍逐字来自 markdown 真源。
    """
    src = root("docs", "HF_COMPOSE.md")
    with open(src, "r", encoding="utf-8") as f:
        md = f.read()
    body = md_to_html(md)
    # 独立站上的相对链接改写为 GitHub 绝对链接（docs-site 不携带仓库
    # 其余文件——README/EXAMPLES 等在站内无对应文件）。
    repo = "https://github.com/samychen/xmedia-kit"
    body = re.sub(r'href="(\.\./[^"]+)"',
                  lambda m: f'href="{repo}/tree/main/'
                            + m.group(1).replace("../", "") + '"', body)
    body = re.sub(r'href="((?!http)[^":][^"]*\.md)"',
                  lambda m: f'href="{repo}/blob/main/docs/'
                            + m.group(1).replace("./", "") + '"', body)
    page = build_landing(body)
    out_dir = root("docs-site")
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, "index.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write(page)
    print(f"xmedia: 文档站已生成 {out}"
          f"（{len(md.splitlines())} 行源 → {len(page.splitlines())} 行 HTML）")


def cmd_doctor(args):
    """环境自检：管线每一步的依赖逐项报告。全部 PASS 才可无人值守。"""
    checks = []

    def check(name, ok, note=""):
        checks.append((name, bool(ok), note))

    check("python ≥ 3.9", sys.version_info >= (3, 9),
          sys.version.split()[0])
    ff = shutil.which("ffprobe") and shutil.which("ffmpeg")
    check("ffmpeg/ffprobe 在 PATH", ff,
          shutil.which("ffprobe") or "缺失（混音/时长探测依赖）")
    if sys.platform == "darwin":
        check("say（macOS 离线 TTS）", os.path.isfile("/usr/bin/say"))
    check("html_bake 已构建", bake_bin() is not None, bake_bin() or
          "缺失（make html_bake）")
    check("GSAP 内联素材", os.path.isfile(root("tests", "testdata",
                                              "gsap.min.js")))
    sfx = [f for f in os.listdir(root("assets", "sfx"))
           if f.endswith(".wav")] if os.path.isdir(root("assets", "sfx")) else []
    check("SFX 素材（assets/sfx）", len(sfx) >= 5,
          f"{len(sfx)} 个音效")
    check("Studio 工具", os.path.isfile(py_tool("hf_studio.py")))
    if os.path.isfile("/usr/bin/say"):
        try:
            voices = subprocess.run(["say", "-v", "?"], capture_output=True,
                                    text=True, timeout=10).stdout
            zh = [l for l in voices.splitlines() if "zh_CN" in l]
            check("中文 TTS 语音", bool(zh), zh[0].split()[0] if zh else
                  "无 zh_CN 语音（系统设置→朗读）")
        except OSError:
            check("中文 TTS 语音", False, "say 探测失败")
    width = max(len(n) for n, _, _ in checks)
    failed = 0
    for name, ok, note in checks:
        mark = "\033[32m✓\033[0m" if ok else "\033[31m✗\033[0m"
        if not ok:
            failed += 1
        print(f"  {mark} {name.ljust(width)}  {note}")
    print(f"\nxmedia doctor: {len(checks) - failed}/{len(checks)} PASS"
          + ("（全部就绪，可无人值守生成）" if failed == 0 else
             "（有缺失项，对应能力不可用）"))
    sys.exit(1 if failed else 0)


def cmd_init(args):
    """脚手架：<dir> 里生成可运行的示例 spec（9:16 三场景）。"""
    os.makedirs(args.dir, exist_ok=True)
    spec_path = os.path.join(args.dir, "demo.json")
    if os.path.exists(spec_path) and not args.force:
        print(f"xmedia: {spec_path} 已存在（--force 覆盖）", file=sys.stderr)
        sys.exit(1)
    with open(spec_path, "w", encoding="utf-8") as f:
        f.write('''{
  "composition": {"id": "demo", "format": "9:16", "fps": 30},
  "background": "#0a0a1a",
  "tts": {"provider": "say", "voice": "Tingting", "rate": 200},
  "scenes": [
    {"type": "title", "text": "欢迎使用 xmedia-kit",
     "subtitle": "AI 视频生成管线", "bg": "#1a1a2e",
     "animation": "fade_in_up"},
    {"type": "stats", "value": "1026", "description": "回归测试全部通过",
     "bg": "#16213e", "animation": "scale_in"},
    {"type": "outro", "text": "谢谢观看", "duration": 2, "bg": "#0f3460"}
  ]
}
''')
    print(f"xmedia: 脚手架就绪 {spec_path}\n"
          f"  下一步：xmedia render {spec_path} --out {args.dir}/demo.mp4")


def cmd_sfx(args):
    """再生成 assets/sfx 音效（与仓库 README 的命令一致）。"""
    sfx_dir = root("assets", "sfx")
    os.makedirs(sfx_dir, exist_ok=True)
    gen = [
        (["-f", "lavfi", "-i", "anoisesrc=d=0.5:c=brown:a=0.8",
          "-af", "lowpass=f=1200,afade=t=in:d=0.15,afade=t=out:st=0.25:"
                 "d=0.25,volume=0.9"], "whoosh.wav"),
        (["-f", "lavfi", "-i", "sine=frequency=880:duration=0.09",
          "-af", "afade=t=out:st=0.02:d=0.07,volume=0.8"], "pop.wav"),
        (["-f", "lavfi", "-i", "sine=frequency=1320:duration=0.5",
          "-af", "afade=t=out:st=0.05:d=0.45,volume=0.7"], "ding.wav"),
        (["-f", "lavfi", "-i", "sine=frequency=660:duration=0.12",
          "-f", "lavfi", "-i", "sine=frequency=990:duration=0.3",
          "-filter_complex", "[0:a][1:a]concat=n=2:v=0:a=1,afade=t=out:"
                             "st=0.25:d=0.15,volume=0.7"], "success.wav"),
        (["-f", "lavfi", "-i", "anoisesrc=d=0.35:c=white:a=0.6",
          "-af", "highpass=f=400,lowpass=f=3000,afade=t=in:d=0.1,"
                 "afade=t=out:st=0.15:d=0.2,volume=0.7"], "transition.wav"),
    ]
    for extra, name in gen:
        run(["ffmpeg", "-y", "-v", "error"] + extra +
            [os.path.join(sfx_dir, name)])
        print(f"xmedia: {name}")


def main():
    parser = argparse.ArgumentParser(
        prog="xmedia",
        description="xmedia-kit AI 视频生成管线统一入口")
    parser.add_argument("--version", action="version", version="xmedia-kit 1.0")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("render", help="全链：TTS→compose→validate→bake")
    p.add_argument("spec", help="spec JSON")
    p.add_argument("--out", default="out.mp4", help="输出 MP4")
    p.add_argument("--no-tts", action="store_true", help="跳过 TTS")
    p.add_argument("--chunks", type=int, default=1)
    p.set_defaults(fn=cmd_render)

    p = sub.add_parser("tts", help="生成旁白（透传 hf_tts.py）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_tts.py")]
                                    + a.args))

    p = sub.add_parser("compose", help="spec → 契约 HTML（透传 hf_compose.py）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_compose.py")]
                                    + a.args))

    p = sub.add_parser("validate", help="契约校验（透传 hf_validate.py）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_validate.py")]
                                    + a.args))

    p = sub.add_parser("studio", help="预览注入 / 本地 Studio 服务（透传）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_studio.py")]
                                    + a.args))

    p = sub.add_parser("import",
                       help="素材目录 → spec（自动排时间轴 / ASR 字幕 / 片尾卡）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_import.py")]
                                    + a.args))

    p = sub.add_parser("asr", help="语音转写 / SRT 导入为分段字幕（透传）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_asr.py")]
                                    + a.args))

    p = sub.add_parser("agent", help="自然语言指令 → spec 变更（自检 / 透传）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("hf_agent.py")]
                                    + a.args))

    p = sub.add_parser("mcp", help="启动 stdio MCP server（供 Claude/Cursor 调用）")
    p.add_argument("args", nargs=argparse.REMAINDER)
    p.set_defaults(fn=lambda a: run([sys.executable, py_tool("xmedia_mcp.py")]
                                    + a.args))

    sub.add_parser("doctor", help="环境自检").set_defaults(fn=cmd_doctor)
    p = sub.add_parser("init", help="脚手架：生成示例 spec")
    p.add_argument("dir")
    p.add_argument("--force", action="store_true")
    p.set_defaults(fn=cmd_init)
    sub.add_parser("sfx", help="再生成 assets/sfx 音效").set_defaults(
        fn=cmd_sfx)
    sub.add_parser("docs", help="生成文档站 docs-site/index.html").set_defaults(
        fn=cmd_docs)

    args = parser.parse_args()
    if getattr(args, "args", None) and args.args and \
            args.args[0] == "--":
        args.args = args.args[1:]
    args.fn(args)


if __name__ == "__main__":
    main()
