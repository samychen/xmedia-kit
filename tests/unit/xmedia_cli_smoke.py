#!/usr/bin/env python3
# xmedia CLI 冒烟（ctest xmedia_cli_smoke）：doctor 环境自检 + init 脚手架
# + render 一条命令全链（TTS→compose→validate→bake），产物含验源锚点。
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "tools", "xmedia.py")


def run(args, **kw):
    proc = subprocess.run([sys.executable, CLI] + args,
                          capture_output=True, text=True, timeout=600, **kw)
    return proc


def main():
    # doctor：全 PASS 才算环境就绪
    proc = run(["doctor"])
    assert proc.returncode == 0, f"doctor 未全过：\n{proc.stdout[-600:]}"
    assert "PASS" in proc.stdout

    with tempfile.TemporaryDirectory(prefix="xmedia_cli_") as tmp:
        # init 脚手架
        proj = os.path.join(tmp, "proj")
        proc = run(["init", proj])
        spec_path = os.path.join(proj, "demo.json")
        assert os.path.isfile(spec_path), proc.stdout
        # 缩短时长加快烘焙
        spec = json.load(open(spec_path, encoding="utf-8"))
        spec["scenes"] = [
            {"type": "title", "text": "CLI 冒烟", "duration": 1.2,
             "bg": "#1a1a2e"},
            {"type": "outro", "text": "结束", "duration": 1.0,
             "bg": "#0f3460"},
        ]
        spec["composition"] = {"id": "cli-smoke", "width": 540,
                               "height": 960, "fps": 30}
        json.dump(spec, open(spec_path, "w", encoding="utf-8"),
                  ensure_ascii=False)

        # render 一条命令全链（TTS→compose→validate→bake）
        out = os.path.join(proj, "demo.mp4")
        proc = run(["render", spec_path, "--out", out])
        assert os.path.isfile(out), f"产物缺失：{proc.stdout[-400:]}"
        assert "xmedia: 完成" in proc.stdout

        # 产物验源锚点：容器 metadata 含工具名
        probe = subprocess.run(
            ["ffprobe", "-show_format", out], capture_output=True,
            text=True, timeout=60)
        assert "xmedia-kit html_bake" in probe.stdout, \
            "MP4 来源锚点缺失"
        assert os.path.isfile(os.path.join(proj, "demo.html_elements.json")), \
            "元素元数据 sidecar 缺失"

    print("xmedia_cli_smoke: PASS")


if __name__ == "__main__":
    main()
