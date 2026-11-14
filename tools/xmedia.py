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


def cmd_docs(args):
    """文档站：docs/HF_COMPOSE.md → docs-site/index.html（样式化单页，
    标题锚点 + 暗色主题与 Studio 同源）。"""
    src = root("docs", "HF_COMPOSE.md")
    with open(src, "r", encoding="utf-8") as f:
        md = f.read()
    body = md_to_html(md)
    page = f'''<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<title>xmedia-kit — AI 视频生成管线文档</title>
<style>
  :root {{ --bg: #0b0b11; --panel: #15151d; --line: #26263a;
          --ink: #e9e9f0; --dim: #9494a8; --acc: #6d8bff; }}
  * {{ box-sizing: border-box; }}
  body {{ margin: 0; background: var(--bg); color: var(--ink);
         font: 15px/1.7 -apple-system, "Helvetica Neue", sans-serif; }}
  .wrap {{ max-width: 920px; margin: 0 auto; padding: 40px 24px 80px; }}
  .top {{ display: flex; align-items: center; gap: 12px; margin-bottom: 8px; }}
  .logo {{ width: 26px; height: 26px; border-radius: 8px;
          background: linear-gradient(135deg, #6d8bff, #8b6dff); }}
  .top b {{ font-size: 15px; }}
  h1 {{ font-size: 30px; border-bottom: 2px solid var(--acc);
       padding-bottom: 12px; }}
  h2 {{ font-size: 22px; margin-top: 44px; border-left: 4px solid var(--acc);
       padding-left: 12px; }}
  h3 {{ font-size: 17px; color: var(--acc); margin-top: 30px; }}
  p, li {{ color: #c9c9d6; }}
  code {{ background: var(--panel); padding: 2px 6px; border-radius: 5px;
         font: 12.5px ui-monospace, Menlo, monospace; color: #9fb4ff; }}
  pre {{ background: var(--panel); border: 1px solid var(--line);
        border-radius: 10px; padding: 14px 16px; overflow-x: auto; }}
  pre code {{ background: none; padding: 0; color: #c9d4ff; }}
  table {{ border-collapse: collapse; width: 100%; margin: 14px 0;
          font-size: 13.5px; }}
  th, td {{ border: 1px solid var(--line); padding: 7px 11px;
           text-align: left; vertical-align: top; }}
  th {{ background: var(--panel); color: var(--ink); }}
  td {{ color: #c9c9d6; }}
  tr:hover td {{ background: var(--panel2); }}
  a {{ color: var(--acc); text-decoration: none; }}
  a:hover {{ text-decoration: underline; }}
  hr {{ border: 0; border-top: 1px solid var(--line); margin: 34px 0; }}
  ul {{ padding-left: 22px; }}
  .foot {{ color: var(--dim); font-size: 12px; margin-top: 60px;
          text-align: center; }}
</style>
</head>
<body><div class="wrap">
<div class="top"><div class="logo"></div><b>xmedia-kit</b></div>
{body}
<div class="foot">xmedia-kit · 本页由 <code>xmedia docs</code> 从
docs/HF_COMPOSE.md 生成</div>
</div></body></html>
'''
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
