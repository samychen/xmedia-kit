#!/usr/bin/env python3
# hf_studio 冒烟（ctest hf_studio_smoke）：契约 HTML → Studio 注入。
# 验证 chrome/驱动注入、场景按钮、幂等原地更新、契约语义不变
# （hf_validate 在 studio 页仍 0 errors、场景 div 原样保留）。
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools")
MARK = "xmedia-kit studio:begin"


def run_tool(args):
    proc = subprocess.run([sys.executable] + args, capture_output=True,
                          text=True, timeout=120)
    assert proc.returncode == 0, f"exit {proc.returncode}\n{proc.stderr[-500:]}"
    return proc


def main():
    with tempfile.TemporaryDirectory(prefix="hf_studio_smoke_") as tmp:
        spec = {
            "composition": {"id": "studio-smoke", "fps": 30},
            "scenes": [
                {"type": "title", "text": "Studio 冒烟", "duration": 2,
                 "bg": "#1a1a2e"},
                {"type": "outro", "text": "结束", "duration": 1.5,
                 "bg": "#0f3460"},
            ],
        }
        spec_path = os.path.join(tmp, "studio_smoke.json")
        with open(spec_path, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False)

        contract = os.path.join(tmp, "studio_smoke.html")
        run_tool([os.path.join(TOOLS, "hf_compose.py"), spec_path,
                  "-o", contract])
        contract_scenes = len(spec["scenes"])

        studio = os.path.join(tmp, "studio_smoke.studio.html")
        run_tool([os.path.join(TOOLS, "hf_studio.py"), contract])
        html = open(studio, encoding="utf-8").read()
        assert html.count(MARK) == 1, "chrome 标记数量不符"
        assert "__mcStudio" in html, "驱动脚本缺失"
        assert html.count('data-goto=') >= 3, "场景跳转按钮缺失"
        # 契约语义不变：场景 div 原样保留、根与 clip 属性未被 chrome 污染
        assert html.count('class="clip scene') == contract_scenes, \
            "场景 div 被改动"
        assert 'data-start="2"' not in html.split("</body>")[1], \
            "chrome 不得携带 data-start（会被可见性切换误伤）"

        # hf_validate 在 studio 页仍 0 errors（chrome 不携带时序属性）
        run_tool([os.path.join(TOOLS, "hf_validate.py"), studio])

        # 幂等原地更新：对 studio 页重复运行 → 单标记、无 .studio.studio 套娃
        run_tool([os.path.join(TOOLS, "hf_studio.py"), studio])
        html2 = open(studio, encoding="utf-8").read()
        assert html2.count(MARK) == 1, "重复注入未做幂等剥离"
        assert not os.path.exists(os.path.join(tmp, "studio_smoke.studio.studio.html")), \
            "产生了套娃文件名"

    print("hf_studio_smoke: PASS")


if __name__ == "__main__":
    main()
