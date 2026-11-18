#!/usr/bin/env python3
# xmedia-kit 契约 HTML 验证器（协议兼容 heygen-com/hyperframes 原版契约）：
# lint 生成/手写的合成 HTML 是否满足渲染契约。
#
# 用法：hf_validate.py <composition.html> [--json]
#
# 检查项（对齐 html_bake 的渲染要求）：
#   1. data-composition-id 根存在
#   2. data-duration 存在且 > 0
#   3. 至少一个 .clip 元素
#   4. __timelines 注册存在（GSAP 时间线）
#   5. GSAP 引用存在（CDN 或内联）
#   6. 无裸 <script src> 指向 file:// 相对路径（CEF OSR 整页黑）

import sys
import os
import re
import argparse
import json


def check_storyboard_discipline(spec, html):
    """storyboard.direction 的纪律检查（全部 warning 级，不阻塞 0-errors
    硬门——纪律是方向，不是渲染错误）：
      * 两色纪律：全局背景 vs ground、场景文字色 vs ink；
      * breather：帧索引越界；
      * 负面清单的机器可查子集：infinite loops（GSAP repeat:-N）、
        randomness（Math.random）——其余条目是视觉概念，不查。
    负面清单只查**作者写的代码**：vendor 引擎（gsap.min.js 内部本就含
    Math.random / repeat:-1）先摘掉，避免引擎实现误报。
    """
    findings = []
    user_html = re.sub(
        r"<script\b[^>]*>(?:(?!</script>).)*?GreenSockGlobals"
        r"(?:(?!</script>).)*?</script>",
        "<script></script>", html, flags=re.S)
    sb = spec.get("storyboard") or {}
    direction = sb.get("direction") or {}
    two = direction.get("two_color") or {}
    ground = two.get("ground")
    ink = two.get("ink")
    scenes = spec.get("scenes") or []
    if ground and spec.get("background") and \
            str(spec["background"]).lower() != str(ground).lower():
        findings.append((
            "warning",
            f"两色纪律：全局背景 {spec['background']} 偏离 "
            f"storyboard.direction.two_color.ground {ground}"))
    if ink:
        bad = [i for i, sc in enumerate(scenes)
               if isinstance(sc, dict) and sc.get("text_color") and
               str(sc["text_color"]).lower() != str(ink).lower()]
        if bad:
            shown = ", ".join("#" + str(i) for i in bad[:5])
            findings.append((
                "warning",
                f"两色纪律：{len(bad)} 个场景文字色偏离 ink {ink}"
                f"（{shown}{'…' if len(bad) > 5 else ''}）"))
    breather = direction.get("breather")
    if breather is not None:
        try:
            bi = int(breather)
        except (TypeError, ValueError):
            bi = -1
        if not (0 <= bi < len(scenes)):
            findings.append((
                "warning",
                f"storyboard.direction.breather={breather} 越界"
                f"（场景数 {len(scenes)}）"))
    banned = [str(b).lower() for b in (direction.get("banned") or [])]
    for label, pat, what in (
            ("infinite loops", r"repeat\s*:\s*-\d+", "GSAP repeat:-N"),
            ("randomness", r"Math\.random", "Math.random")):
        if any(label in b for b in banned) and re.search(pat, user_html):
            findings.append((
                "warning", f"负面清单「{label}」：契约页含 {what}"))
    return findings


def validate(html, spec=None):
    findings = []
    errors = 0
    warnings = 0
    infos = 0

    # 1. composition root
    if not re.search(r'data-composition-id=', html):
        findings.append(("error", "缺少 data-composition-id 合成根"))
        errors += 1
    else:
        findings.append(("info", "data-composition-id 合成根 ✓"))
        infos += 1

    # 2. duration
    m = re.search(r'data-duration="([\d.]+)"', html)
    if not m:
        findings.append(("error", "合成根缺少 data-duration"))
        errors += 1
    elif float(m.group(1)) <= 0:
        findings.append(("error", f"data-duration={m.group(1)} 必须 > 0"))
        errors += 1
    else:
        findings.append(("info", f"data-duration={m.group(1)} ✓"))
        infos += 1

    # 3. clip elements
    clips = re.findall(r'data-start="([\d.]+)"', html)
    if not clips:
        findings.append(("warning", "无 data-start 元素（无时序 clip）"))
        warnings += 1
    else:
        findings.append(("info", f"{len(clips)} 个 data-start clip ✓"))
        infos += 1

    # 4. __timelines registration
    if "__timelines" not in html:
        findings.append(("error", "缺少 window.__timelines 注册（GSAP 时间线）"))
        errors += 1
    else:
        findings.append(("info", "__timelines 注册 ✓"))
        infos += 1

    # 5. GSAP reference
    if "gsap" not in html.lower():
        findings.append(("error", "缺少 GSAP 引用（动画引擎）"))
        errors += 1
    else:
        findings.append(("info", "GSAP 引用 ✓"))
        infos += 1

    # 6. file:// relative script src（CEF OSR 整页黑）
    for m in re.finditer(r'<script\s+src="([^"]+)"', html):
        src = m.group(1)
        if not src.startswith(("http://", "https://", "//")) and not src.startswith("/"):
            findings.append(("warning",
                             f'<script src="{src}"> 相对路径 file:// 子资源'
                             "在 CEF OSR 整页黑——建议内联或用 CDN"))
            warnings += 1

    # 7. storyboard 叙事纪律（可选 spec；全部 warning 级）
    if spec and spec.get("storyboard"):
        for lv, msg in check_storyboard_discipline(spec, html):
            findings.append((lv, msg))
            warnings += 1

    return findings, errors, warnings, infos


def main():
    parser = argparse.ArgumentParser(description="xmedia-kit 契约 HTML 验证器")
    parser.add_argument("html", help="合成 HTML 文件路径")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    parser.add_argument("--spec", default="",
                        help="spec JSON 路径（含 storyboard 段时做叙事纪律"
                             "检查；缺省按契约页头注释自动发现）")
    args = parser.parse_args()

    try:
        with open(args.html, "r", encoding="utf-8") as f:
            html = f.read()
    except FileNotFoundError:
        print(f"hf_validate: HTML 不存在：{args.html}\n"
              "  先跑 hf_compose.py 生成契约 HTML", file=sys.stderr)
        sys.exit(1)

    spec = None
    spec_path = args.spec
    if not spec_path:
        # 自动发现：compose 在头注释写 spec:<文件名>|（相对契约页目录）
        m = re.search(r"spec:([^|]+)\|", html)
        if m:
            cand = os.path.join(os.path.dirname(os.path.abspath(args.html)),
                                m.group(1).strip())
            if os.path.isfile(cand):
                spec_path = cand
    if spec_path and os.path.isfile(spec_path):
        try:
            spec = json.load(open(spec_path, encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            spec = None

    findings, errors, warnings, infos = validate(html, spec=spec)

    if args.json:
        print(json.dumps({
            "errorCount": errors, "warningCount": warnings,
            "infoCount": infos,
            "findings": [{"level": lv, "message": msg}
                         for lv, msg in findings],
        }))
    else:
        for lv, msg in findings:
            icon = {"error": "✗", "warning": "⚠", "info": "ℹ"}.get(lv, "•")
            print(f"  {icon} [{lv}] {msg}")
        print(f"\n{errors} errors, {warnings} warnings, {infos} infos")
        sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
