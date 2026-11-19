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
    """html_bake 可执行文件：macOS bundle 优先，Linux 裸二进制兜底。

    搜索顺序：build/bin/（顶层构建）→ build/<preset>/bin/（CMakePresets 的
    每个预设各一棵树，CI 与本地多配置并存时二进制在这里）。预设目录按名字
    排序，保证同一仓库多棵树时结果确定；返回第一个可执行的。
    """
    rel = [("html_bake.app", "Contents", "MacOS", "html_bake"), ("html_bake",)]
    cands = [root("build", "bin", *r) for r in rel]
    build_root = root("build")
    if os.path.isdir(build_root):
        for name in sorted(os.listdir(build_root)):
            if os.path.isdir(os.path.join(build_root, name, "bin")):
                cands.extend(os.path.join(build_root, name, "bin", *r)
                             for r in rel)
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
        "官方 CLI 示例：标题 + 数据场景、TTS 旁白与导出 MP4。",
        ("9:16", "TTS", "入门")),
}

# Hero 三段介绍（对位 Remotion 官网 hero 下的三栏切换轮播）。
INTRO_SLIDES = (
    ("product-launch", "写一份 spec",
     "画幅、场景、动画与时序都声明在 JSON 里；改一个字段就是一条新片。"),
    ("knowledge-cards", "帧级预览",
     "Studio 等比缩放画布、逐帧步进，播放头与场景 / 属性双向联动。"),
    ("event-invite", "确定性导出",
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
    # tools/gsap.min.js 是随包分发的那份（npm 用户没有 tests/）；
    # tests/testdata/ 那份是源码树的原始位置。两者有其一即可。
    gsap = [root("tools", "gsap.min.js"),
            root("tests", "testdata", "gsap.min.js")]
    check("GSAP 内联素材", any(os.path.isfile(g) for g in gsap),
          next((g for g in gsap if os.path.isfile(g)), "缺失（动画不会跑）"))
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


LAUNCH_SLOT_BG = {
    "hook": "#1a1a2e", "proof": "#16213e",
    "turn": "#0f3460", "cta": "#1a1a2e",
}


def _launch_fetch_url(url):
    """尽力抓取网站文本（<title> + 正文标签内文字）；失败返回空串。"""
    try:
        import urllib.request
        req = urllib.request.Request(
            url, headers={"User-Agent": "xmedia-launch/1.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            html = resp.read(256 * 1024).decode("utf-8", "replace")
    except (OSError, ValueError):
        return ""
    title = re.search(r"<title[^>]*>([^<]*)</title>", html, re.I)
    parts = [t.strip() for t in (title.group(1) if title else "").split()]
    # 正文：剥掉 <script>/<style> 后抽常见容器标签内的文字。
    body = re.sub(r"<(script|style)[^>]*>.*?</\1>", "", html,
                  flags=re.I | re.S)
    for tag in ("h1", "h2", "p", "li"):
        for m in re.finditer(rf"<{tag}[^>]*>(.*?)</{tag}>", body,
                             re.I | re.S):
            txt = re.sub(r"<[^>]+>", " ", m.group(1))
            txt = re.sub(r"\s+", " ", txt).strip()
            if 12 <= len(txt) <= 160:
                parts.append(txt)
        if len(parts) >= 14:
            break
    return "\n".join(parts[:14])


def _brand_dir():
    return os.path.expanduser(os.environ.get(
        "XMEDIA_BRAND_DIR", "~/.xmedia/brands"))


def cmd_storyboard(args):
    """叙事计划（Storyboard）→ spec：HF Storyboard 工作流的本地版。

    Arc = 商业叙事的节拍骨架（每帧带 type/persuasion/beat 词汇）；
    --message/--audience/--mood 是 HF 的叙事计划四要素之三（第四个是
    Arc 本身）。产出的 spec.storyboard 段供 Studio 分镜面板显示叙事
    徽标、供 agent 模式读方向块纪律（两色/breather/负面清单）。
    """
    import hf_storyboard
    if not args.arc or args.arc == "list":
        print("xmedia: 商业叙事 Arc 模板（storyboard <arc> --message ...）：")
        for a in hf_storyboard.arcs():
            print(f"  {a['id']:<17} {a['frames']} 帧  {a['label']}")
        print("  要素：--message 一句话论点 / --audience 受众 / --mood "
              "音乐能量（HF 叙事计划四要素，第四个是 Arc 本身）")
        return
    if args.arc not in hf_storyboard.ARCS:
        print(f"xmedia: 未知 Arc「{args.arc}」（storyboard list 查看全部）",
              file=sys.stderr)
        sys.exit(1)
    if not args.message:
        print("xmedia: 缺 --message（整片的一句话论点）", file=sys.stderr)
        sys.exit(1)
    spec = hf_storyboard.build(
        args.arc, args.message, audience=args.audience, mood=args.mood,
        fmt=args.format, fps=args.fps, voice=args.voice)
    if args.name:
        spec["composition"]["id"] = args.name
    out = os.path.abspath(args.out or
                          (args.arc + "-storyboard.json"))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
        f.write("\n")
    arc_def = hf_storyboard.ARCS[args.arc]
    print(f"xmedia: 叙事计划就绪 {out}\n"
          f"  Arc「{arc_def['label']}」· message「{args.message[:36]}」"
          f"· 受众「{args.audience or '未设'}」\n"
          f"  {len(spec['scenes'])} 帧（每帧 type/persuasion/beat 见 "
          f"spec.storyboard.frames，Studio 分镜面板显示徽标）\n"
          f"  下一步：改占位文案（Studio 分镜卡内联编辑/Ask AI）→ "
          f"xmedia render {out} --out {os.path.splitext(out)[0]}.mp4")


def cmd_brand(args):
    """品牌套件：色板/字体色/音色/片尾署名的持久化与注入 spec。"""
    d = _brand_dir()
    if args.action == "list":
        if not os.path.isdir(d):
            print("xmedia: 无已保存品牌（先 brand save）")
            return
        for f in sorted(os.listdir(d)):
            if f.endswith(".json"):
                print(" ", f[:-5])
        return
    if args.action == "scan":
        if not args.name or not args.url:
            print("xmedia: brand scan 需要 <名> --url <网站>", file=sys.stderr)
            sys.exit(1)
        draft = _brand_scan(args.url)
        brand = {
            "name": args.name,
            "background": draft["background"],
            "palette": draft["palette"],
            "text_color": draft["text_color"] or "#f0f0f5",
            "voice": args.voice or "Tingting",
            "outro_text": "",
            "logo_text": (draft["site_title"] or args.name)[:24],
            "fonts": draft["fonts"],
            "logo_image_url": draft["logo_image_url"],
            "source_url": draft["source_url"],
        }
        d = _brand_dir()
        os.makedirs(d, exist_ok=True)
        path = os.path.join(d, args.name + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(brand, f, ensure_ascii=False, indent=1)
            f.write("\n")
        print(f"xmedia: 品牌「{args.name}」扫描自 {draft['source_url']}")
        print(f"  色板   {' '.join(brand['palette'])}"
              f"（背景 {brand['background']}，文字 {brand['text_color']}）")
        if brand["fonts"]:
            print(f"  字体   {', '.join(brand['fonts'])}")
        if brand["logo_image_url"]:
            print(f"  logo   {brand['logo_image_url']}")
        logo_local = ""
        if brand["logo_image_url"] and brand["logo_image_url"].startswith(
                ("http://", "https://")):
            try:
                import urllib.request
                req = urllib.request.Request(
                    brand["logo_image_url"],
                    headers={"User-Agent": "Mozilla/5.0 (xmedia-kit brand scan)"})
                with urllib.request.urlopen(req, timeout=10) as r:
                    data = r.read(4_000_000)
                ext = os.path.splitext(
                    urllib.parse.urlparse(brand["logo_image_url"]).path)[1] \
                    or ".png"
                assets = os.path.join(_brand_dir(), "assets")
                os.makedirs(assets, exist_ok=True)
                dest = os.path.join(assets, args.name + "-logo" + ext)
                with open(dest, "wb") as f:
                    f.write(data)
                logo_local = os.path.relpath(dest, _brand_dir())
                brand["logo_image"] = logo_local
                with open(path, "w", encoding="utf-8") as f:
                    json.dump(brand, f, ensure_ascii=False, indent=1)
                    f.write("\n")
            except Exception as e:
                print(f"  logo 下载失败（保留 URL）：{e}", file=sys.stderr)
        if logo_local:
            print(f"  logo   {brand['logo_image_url']} → {logo_local}")
        print(f"  已保存 {path} —— brand apply {args.name} --spec <spec> 生效")
        return
    if args.action == "save":
        os.makedirs(d, exist_ok=True)
        # v2 结构（对齐 HF brand kit：colors with roles / ink / fonts
        # with weight / rules Do's-Don'ts）；v1 兼容字段（palette/
        # text_color）同时保留给旧读取方。
        palette = [args.palette1 or "#1a1a2e",
                   args.palette2 or "#16213e",
                   args.palette3 or "#0f3460"]
        ink = args.text or "#f0f0f5"
        brand = {"name": args.name,
                 "revision": 1,
                 "background": args.background or "#0a0a1a",
                 "colors": [{"hex": palette[0], "role": "ground"},
                            {"hex": palette[1], "role": "surface"},
                            {"hex": palette[2], "role": "accent"}],
                 "palette": palette,
                 "ink": ink,
                 "text_color": ink,
                 "fonts": {"body": {"family": args.font_family or "",
                                    "weight": 400}},
                 "font_family": args.font_family or "",
                 "voice": args.voice or "Tingting",
                 "outro": {"text": args.outro or "",
                           "logo_text": args.logo or ""},
                 "outro_text": args.outro or "",
                 "logo_text": args.logo or "",
                 "rules": {"dos": [], "donts": []}}
        if args.logo_image:
            if not os.path.isfile(args.logo_image):
                print(f"xmedia: logo 图片不存在 {args.logo_image}",
                      file=sys.stderr)
                sys.exit(1)
            assets = os.path.join(d, "assets")
            os.makedirs(assets, exist_ok=True)
            ext = os.path.splitext(args.logo_image)[1] or ".png"
            dest = os.path.join(assets, args.name + "-logo" + ext)
            shutil.copyfile(args.logo_image, dest)
            brand["logo_image"] = os.path.relpath(dest, d)
        path = os.path.join(d, args.name + ".json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(brand, f, ensure_ascii=False, indent=1)
            f.write("\n")
        print(f"xmedia: 品牌「{args.name}」已保存 {path}")
        return
    if args.action == "default":
        pin = os.path.join(d, ".default")
        if args.name:
            if not os.path.exists(os.path.join(d, args.name + ".json")):
                print(f"xmedia: 品牌「{args.name}」不存在", file=sys.stderr)
                sys.exit(1)
            with open(pin, "w", encoding="utf-8") as f:
                f.write(args.name)
            print(f"xmedia: 默认品牌已设为「{args.name}」（新项目自动应用）")
        else:
            if os.path.isfile(pin):
                print("xmedia: 默认品牌 =",
                      open(pin, encoding="utf-8").read().strip())
            else:
                print("xmedia: 未设置默认品牌（brand default <名> 设置）")
        return
    if args.action == "show":
        path = os.path.join(d, args.name + ".json")
        if not os.path.exists(path):
            print(f"xmedia: 品牌「{args.name}」不存在（brand list 查看）",
                  file=sys.stderr)
            sys.exit(1)
        print(open(path, encoding="utf-8").read())
        return
    # apply（注入逻辑统一走 hf_agent.apply_brand——与 Studio 指令/Home
    # 默认品牌同一份逻辑）
    path = os.path.join(d, args.name + ".json")
    if not os.path.exists(path):
        print(f"xmedia: 品牌「{args.name}」不存在（brand list 查看）",
              file=sys.stderr)
        sys.exit(1)
    brand = json.load(open(path, encoding="utf-8"))
    if not os.path.exists(args.spec):
        print(f"xmedia: spec 不存在 {args.spec}", file=sys.stderr)
        sys.exit(1)
    spec = json.load(open(args.spec, encoding="utf-8"))
    sys.path.insert(0, TOOLS_DIR)
    import hf_agent
    logo_src = ""
    if brand.get("logo_image"):
        # 品牌资产目录里的 logo → 绝对路径（契约页 file:// 可读）
        cand = os.path.join(d, brand["logo_image"])
        logo_src = os.path.abspath(cand) if os.path.isfile(cand) else \
            brand["logo_image"]
    brand["logo_src"] = logo_src
    spec, notes = hf_agent.apply_brand(spec, brand)
    with open(args.spec, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"xmedia: 品牌「{args.name}」已应用 {args.spec}")
    for n in notes:
        print(f"  {n}")


def _brand_scan(url):
    """读网站 → 品牌草稿（色板/字体/站点名/logo 图）。

    纯本地解析（对齐 HF brand-kits 的"输入网站自动读品牌"）：抓页面 +
    至多 3 个同源样式表，统计 CSS 颜色与 font-family 频次。启发式：
    色板取饱和度优先的高频色（互相色距过近的合并），背景取最常见浅色
    或深色，文字色取与背景对比最大的近黑/近白。读不到就用 CLI 给的
    默认值——扫描是草稿，brand save 的参数永远可以覆盖。
    """
    import urllib.request
    import urllib.parse
    import html as _html

    def _fetch(u, timeout=10):
        req = urllib.request.Request(
            u, headers={"User-Agent": "Mozilla/5.0 (xmedia-kit brand scan)"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.read(2_000_000).decode("utf-8", "replace")

    def _abs(u, base):
        return urllib.parse.urljoin(base, u)

    if url.startswith("file://"):
        html_src = _fetch(url)
        base = url
    else:
        if not url.startswith(("http://", "https://")):
            url = "https://" + url
        html_src = _fetch(url)
        base = url

    def _tag_attr(pattern):
        m = re.search(pattern, html_src, re.I)
        return _html.unescape(m.group(1)).strip() if m else ""

    site_title = (_tag_attr(r'<meta\s+property="og:site_name"\s+content="([^"]*)"')
                  or _tag_attr(r"<title[^>]*>([^<]*)</title>"))
    logo_url = (_tag_attr(r'<meta\s+property="og:image"\s+content="([^"]*)"')
                or _tag_attr(r'<link\s+rel="apple-touch-icon"[^>]*href="([^"]*)"'))

    # 同源样式表（至多 3 个，每个 ≤1MB）：外链 CSS 是颜色的大头
    css_chunks = re.findall(r"<style[^>]*>(.*?)</style>", html_src, re.I | re.S)
    for href in re.findall(r'<link[^>]+rel="stylesheet"[^>]*href="([^"]*)"',
                           html_src, re.I)[:3]:
        absu = _abs(_html.unescape(href), base)
        if absu.startswith(("http://", "https://", "file://")):
            try:
                css_chunks.append(_fetch(absu)[:1_000_000])
            except Exception:
                pass  # 样式表读不到不影响其余启发式
    hay = "\n".join(css_chunks) + "\n" + " ".join(
        re.findall(r'style="([^"]*)"', html_src))

    def _norm_hex(c):
        c = c.lower()
        if re.fullmatch(r"#[0-9a-f]{3}", c):
            c = "#" + "".join(ch * 2 for ch in c[1:])
        return c

    def _css_color_to_hex(c):
        c = c.strip().lower()
        if c.startswith("#"):
            return _norm_hex(c)
        m = re.fullmatch(r"rgba?\(\s*(\d+)[,\s]+(\d+)[,\s]+(\d+)"
                         r"(?:[,\s/]+([0-9.]+))?\s*\)", c)
        if not m:
            return None
        r_, g_, b_, a = int(m.group(1)), int(m.group(2)), int(m.group(3)), m.group(4)
        if a is not None and float(a) < 0.1:
            return None  # 近全透明不算色
        return f"#{r_:02x}{g_:02x}{b_:02x}"

    freq = {}
    for raw in re.findall(r"#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)", hay):
        hx = _css_color_to_hex(raw)
        if hx and re.fullmatch(r"#[0-9a-f]{6}", hx):
            freq[hx] = freq.get(hx, 0) + 1

    def _chan(hx):
        return tuple(int(hx[i:i + 2], 16) for i in (1, 3, 5))

    def _sat(hx):
        r_, g_, b_ = _chan(hx)
        return max(r_, g_, b_) - min(r_, g_, b_)

    def _dist(a, b):
        ca, cb = _chan(a), _chan(b)
        return max(abs(x - y) for x, y in zip(ca, cb))

    colors = sorted(freq.items(), key=lambda kv: (-_sat(kv[0]), -kv[1]))
    palette, bg, text = [], None, None
    for hx, _n in colors:
        if len(palette) < 3 and _sat(hx) >= 24 and \
                all(_dist(hx, p) > 32 for p in palette):
            palette.append(hx)
        if bg is None and (_chan(hx)[0] > 235 and _chan(hx)[1] > 235
                           or max(_chan(hx)) < 40):
            if freq[hx] >= 2:
                bg = hx
    if bg:
        # 文字色必须与背景对比：浅底配近黑、深底配近白，且色距够大
        light_bg = max(_chan(bg)) > 128
        best, best_f = None, 0
        for hx, n in freq.items():
            ch = _chan(hx)
            ok_polarity = (min(ch) > 200 and max(ch) > 200) if light_bg \
                else (max(ch) < 90 and min(ch) < 90)
            if ok_polarity and _dist(hx, bg) > 150 and n > best_f:
                best, best_f = hx, n
        text = best or ("#111318" if light_bg else "#f0f0f5")
    if len(palette) < 3:
        fallback = ["#1a1a2e", "#16213e", "#0f3460"]
        palette += fallback[len(palette):]

    fonts = []
    for decl in re.findall(r"font-family\s*:\s*([^;}]+)", hay, re.I):
        for fam in decl.split(","):
            fam = fam.strip().strip("'\"").replace("!important", "").strip()
            if fam and fam.lower() not in [f.lower() for f in fonts]:
                fonts.append(fam)
    fonts = fonts[:4]

    return {"palette": palette, "background": bg or "#0a0a1a",
            "text_color": text or "", "fonts": fonts,
            "logo_image_url": _abs(logo_url, base) if logo_url else "",
            "site_title": site_title, "source_url": url}


def cmd_reference(args):
    """参考视频 → 同节奏 spec 骨架（reference-video 本地弱化版）。"""
    import hf_reference
    try:
        spec_path, report = hf_reference.run(
            args.video, out=args.out or None, max_scenes=args.max_scenes,
            threshold=args.threshold, min_shot=args.min_shot,
            do_asr=args.asr, name=args.name or None)
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"xmedia: {e}", file=sys.stderr)
        sys.exit(2)
    print(f"xmedia: {report['shots']} 镜 / {report['duration']}s"
          f" / 缩略图 {report['thumbs']} 张"
          f"{' / 参考字幕已生成' if report['captions'] else ''}")
    if report["asr_note"]:
        print(f"xmedia: ASR 跳过——{report['asr_note']}", file=sys.stderr)
    print(f"  spec     {report['spec']}")
    print(f"  对照表   {report['shots_json']}")
    print("  下一步：改 spec 里的占位文案/换成自己的素材 → xmedia render")


def cmd_launch(args):
    """四段式 launch 视频 spec（对齐 HF first-video：Hook/Proof/Turn/CTA）。"""
    missing = [k for k, v in (("hook", args.hook), ("proof", args.proof),
                              ("turn", args.turn), ("cta", args.cta))
               if not v]
    if missing:
        print("xmedia: 缺少 slot 文本：--" + " --".join(missing), file=sys.stderr)
        sys.exit(1)
    # --url 尽力读取网站文案作补充素材；读不到就用 --idea 或纯 slot 文本。
    site_text = ""
    if args.url:
        site_text = _launch_fetch_url(args.url)
        print(f"xmedia: 网站 {'已读取' if site_text else '读取失败（仅用 slot 文本）'}: "
              f"{args.url}")
    idea = (args.idea or site_text.split("\n")[0] if site_text
            else (args.idea or ""))
    idea = idea or (site_text.split("\n")[0] if site_text else "")
    if args.format == "9:16":
        w, h = 1080, 1920
    elif args.format == "1:1":
        w, h = 1080, 1080
    else:
        w, h = 1920, 1080
    scenes = [
        {"type": "title", "text": args.hook, "subtitle": idea,
         "bg": LAUNCH_SLOT_BG["hook"], "animation": "fade_in_up"},
        {"type": "stats", "value": args.proof.split("|")[0].strip(),
         "description": args.proof.split("|")[1].strip()
         if "|" in args.proof else "已验证",
         "bg": LAUNCH_SLOT_BG["proof"], "animation": "scale_in"},
        {"type": "quote", "text": args.turn,
         "bg": LAUNCH_SLOT_BG["turn"], "animation": "fade_in_up"},
        {"type": "outro", "text": args.cta, "duration": 2.5,
         "bg": LAUNCH_SLOT_BG["cta"]},
    ]
    if site_text and args.include_site:
        scenes.insert(3, {"type": "list", "text": "来自网站",
                          "items": [x.strip()[:38] for x in
                                    site_text.split("\n")[:3] if x.strip()],
                          "bg": "#16213e", "animation": "scale_in"})
    spec = {
        "composition": {"id": args.name, "format": args.format,
                        "fps": args.fps, "width": w, "height": h},
        "background": "#0a0a1a",
        "tts": {"provider": "say", "voice": args.voice, "rate": 200},
        "scenes": scenes,
    }
    if args.url:
        spec["launch_url"] = args.url
    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print(f"xmedia: launch spec 就绪 {args.out}\n"
          f"  四段：Hook「{args.hook[:24]}」→ Proof → Turn → CTA「{args.cta[:24]}」\n"
          f"  下一步：xmedia render {args.out} --out "
          f"{os.path.splitext(args.out)[0]}.mp4\n"
          f"  或微调后：xmedia studio {os.path.splitext(args.out)[0]}.html "
          f"--serve --spec {args.out}")


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
    p = sub.add_parser("launch",
                       help="四段式 launch 视频 spec（Hook/Proof/Turn/CTA）")
    p.add_argument("--hook", required=True, help="钩子：一句话抓注意力")
    p.add_argument("--proof", required=True,
                   help="证明：数据/里程碑，用「值|说明」给出")
    p.add_argument("--turn", required=True, help="转折：差异化主张")
    p.add_argument("--cta", required=True, help="行动号召：片尾文案")
    p.add_argument("--url", help="产品网站：尽力读取文案作补充素材")
    p.add_argument("--idea", help="一句话想法（作副标题；默认取网站首行）")
    p.add_argument("--include-site", action="store_true",
                   help="把网站文案要点插入为 list 场景")
    p.add_argument("--name", default="launch", help="合成 id（默认 launch）")
    p.add_argument("--format", default="9:16",
                   choices=["9:16", "16:9", "1:1"])
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--voice", default="Tingting", help="TTS 音色")
    p.add_argument("--out", required=True, help="spec JSON 输出路径")
    p.set_defaults(fn=cmd_launch)
    p = sub.add_parser(
        "reference",
        help="参考视频 → 同节奏 spec 骨架（reference-video 本地弱化版）")
    p.add_argument("video", help="参考视频文件（本地文件）")
    p.add_argument("-o", "--out", help="输出目录（默认 reference/<视频名>/）")
    p.add_argument("--max-scenes", type=int, default=12,
                   help="场景数上限（默认 12）")
    p.add_argument("--threshold", type=float, default=0.3,
                   help="场景切换检测阈值 0-1（默认 0.3）")
    p.add_argument("--min-shot", type=float, default=0.6,
                   help="最短镜头秒数（默认 0.6）")
    p.add_argument("--asr", action="store_true",
                   help="用 hf_asr 把参考音频转成全局字幕段")
    p.add_argument("--name", default="", help="合成 id")
    p.set_defaults(fn=cmd_reference)
    p = sub.add_parser("brand", help="品牌套件：save/list/show/apply/scan/default")
    p.add_argument("action",
                   choices=["save", "list", "show", "apply", "scan",
                            "default"])
    p.add_argument("name", nargs="?", help="品牌名")
    p.add_argument("--spec", help="apply 目标 spec JSON")
    p.add_argument("--logo-mode", choices=["corner", "backdrop"],
                   help="logo 图注入模式（默认 corner 角标；backdrop 铺满置底）")
    p.add_argument("--url", help="scan：要读取的网站（读色板/字体/站点名）")
    p.add_argument("--background", help="全局背景色")
    p.add_argument("--palette1", help="场景色 1")
    p.add_argument("--palette2", help="场景色 2")
    p.add_argument("--palette3", help="场景色 3")
    p.add_argument("--text", help="文字色")
    p.add_argument("--voice", help="TTS 音色")
    p.add_argument("--outro", help="片尾文案")
    p.add_argument("--logo", help="片尾署名")
    p.add_argument("--logo-image", help="logo 图片文件（复制进品牌 assets，apply 时注入场景）")
    p.add_argument("--font-family", help="正文字体族（apply 时写 spec.fonts.body）")
    p.set_defaults(fn=cmd_brand)
    p = sub.add_parser("storyboard",
                       help="叙事计划 → spec：商业 Arc 模板库"
                            "（launch/problem-solution/listicle/explainer/"
                            "comparison；storyboard list 查看详情）")
    p.add_argument("arc", nargs="?",
                   help="Arc 名；缺省或 list = 列出全部 Arc")
    p.add_argument("--message", default="",
                   help="一句话论点（整片的 message，占位文案由此推导）")
    p.add_argument("--audience", default="", help="受众短语")
    p.add_argument("--mood", default="", help="音乐/能量描述")
    p.add_argument("--name", default=None, help="合成 id")
    p.add_argument("--format", default="9:16",
                   choices=["9:16", "16:9", "1:1"])
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--voice", default="Tingting", help="TTS 音色")
    p.add_argument("-o", "--out", default=None, help="spec 输出路径")
    p.set_defaults(fn=cmd_storyboard)
    sub.add_parser("docs", help="生成文档站 docs-site/index.html").set_defaults(
        fn=cmd_docs)

    args = parser.parse_args()
    if getattr(args, "args", None) and args.args and \
            args.args[0] == "--":
        args.args = args.args[1:]
    args.fn(args)


if __name__ == "__main__":
    main()
