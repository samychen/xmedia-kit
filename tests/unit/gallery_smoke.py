#!/usr/bin/env python3
# gallery 冒烟（ctest gallery_smoke）：示例库每个 spec 走 compose +
# validate，0 errors 才算交付质量——示例库烂掉 = 生态门面烂掉。
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GALLERY = os.path.join(ROOT, "gallery")
specs = sorted(glob.glob(os.path.join(GALLERY, "*.json")))
assert len(specs) >= 3, f"示例库 spec 少于 3 个：{specs}"

for spec in specs:
    html = spec.replace(".json", ".html")
    for tool_args in (
        [os.path.join(ROOT, "tools", "hf_compose.py"), spec, "-o", html],
        [os.path.join(ROOT, "tools", "hf_validate.py"), html],
    ):
        proc = subprocess.run([sys.executable] + tool_args,
                              capture_output=True, text=True, timeout=120)
        assert proc.returncode == 0, (
            f"{os.path.basename(spec)} 步骤失败：\n{proc.stderr[-400:]}")
    html_text = open(html, encoding="utf-8").read()
    assert "| schema:1.0" in html_text, f"{spec} 缺 schema 锚点"
    assert html_text.count("tl.fromTo") >= 2, f"{spec} 时间线场景过少"
    os.remove(html)

print(f"gallery_smoke: PASS — {len(specs)} 个示例全部 compose+validate")
