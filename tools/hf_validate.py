#!/usr/bin/env python3
# HyperFrames 契约 HTML 验证器：lint 生成/手写的合成 HTML 是否满足渲染契约。
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
import re
import argparse


def validate(html):
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

    return findings, errors, warnings, infos


def main():
    parser = argparse.ArgumentParser(description="HyperFrames 契约 HTML 验证器")
    parser.add_argument("html", help="合成 HTML 文件路径")
    parser.add_argument("--json", action="store_true", help="JSON 输出")
    args = parser.parse_args()

    with open(args.html, "r", encoding="utf-8") as f:
        html = f.read()

    findings, errors, warnings, infos = validate(html)

    if args.json:
        import json
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
