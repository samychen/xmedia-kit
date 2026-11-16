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
    # 本用例要跑通 render（含烘焙）。没有 html_bake 时先跳过：doctor 会因此
    # 报 7/8，那是正确的环境判定，不该被当成回归。干净 clone 上首次
    # ctest 就是这个状态。
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    import xmedia  # noqa: E402
    if not xmedia.bake_bin():
        print("xmedia_cli_smoke: SKIP — html_bake 未构建"
              "（cmake --build build --target html_bake）")
        return 0

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

        # docs 子命令：文档站与真源同步 + 落地页静态契约
        proc = run(["docs"])
        site = os.path.join(ROOT, "docs-site", "index.html")
        assert os.path.isfile(site), "docs-site/index.html 缺失"
        html = open(site, encoding="utf-8").read()
        assert "xmedia-kit AI 视频生成管线" in html and \
            "自定义模板协议" in html, "文档站关键章节缺失"
        assert "文档站与作品轮播" in html, "新增文档章节未同步"
        for section in ("AI 能力集成", "ASR 分段字幕", "素材自动导入",
                        "Ask AI 与 Agent", "MCP server"):
            assert section in html, f"AI 能力文档章节未同步：{section}"
        for token in ('class="stage"', 'id="reel-tabs"',
                      'id="stage-cover"', 'id="works-track"',
                      'id="doc-body"', 'class="toc"',
                      'var SLIDES = [', '<article class="work-card'):
            assert token in html, f"落地页缺失：{token}"
        assert "@@BODY@@" not in html, "模板占位符未替换"
        # 模板注释里不能出现锚点字面量，否则数据会被注入两次。
        assert html.count('<article class="work-card') <= 6, \
            "作品卡片重复注入"
        assert html.count('var SLIDES = [') == 1, "轮播数据重复注入"
        # docs 幂等：重复生成逐字一致（预览素材按 mtime 缓存）。
        run(["docs"])
        assert open(site, encoding="utf-8").read() == html, \
            "xmedia docs 非幂等"

        # AI 能力 CLI 注册：import / asr / agent / mcp 必须随统一入口发布
        cli_help = run(["--help"]).stdout
        for cmd in ("import", "asr", "agent", "mcp"):
            assert cmd in cli_help, f"xmedia CLI 缺少 {cmd} 子命令"
        for mod in ("hf_asr.py", "hf_import.py", "hf_agent.py",
                    "xmedia_mcp.py"):
            assert os.path.isfile(os.path.join(ROOT, "tools", mod)), \
                f"AI 模块缺失：{mod}"

    print("xmedia_cli_smoke: PASS")


if __name__ == "__main__":
    main()
