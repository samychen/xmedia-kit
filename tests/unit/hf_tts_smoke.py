#!/usr/bin/env python3
# hf_tts / hf_compose 链路冒烟（ctest hf_tts_smoke）：本地 HTTP mock 充当
# TTS 服务（POST {text,voice,rate} → 固定 wav 字节），不依赖 say/网络——
# http provider、类型聚合（stats 无 text 不漏旁白）、幂等复跑、compose
# 拟合与 <audio> 发射全部走真工具进程。判定与工具自身的 ffprobe 探测
# 一致，产物落 tempdir。
import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools")
SAMPLE_WAV = os.path.join(ROOT, "tests", "testdata", "tts", "narr_sample_1.wav")


def run_tool(args):
    proc = subprocess.run([sys.executable] + args, capture_output=True,
                          text=True, timeout=120)
    assert proc.returncode == 0, f"exit {proc.returncode}\n{proc.stderr[-500:]}"
    return proc


def main():
    with open(SAMPLE_WAV, "rb") as f:
        wav = f.read()

    class MockTTS(BaseHTTPRequestHandler):
        def do_POST(self):
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)
            assert b"text" in body, "服务端契约：请求体须含 text 字段"
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(wav)))
            self.end_headers()
            self.wfile.write(wav)

        def log_message(self, *args):
            pass

    server = HTTPServer(("127.0.0.1", 0), MockTTS)
    endpoint = f"http://127.0.0.1:{server.server_address[1]}/tts"
    threading.Thread(target=server.serve_forever, daemon=True).start()

    with tempfile.TemporaryDirectory(prefix="hf_tts_smoke_") as tmp:
        spec = {
            "composition": {"id": "tts-smoke", "fps": 30},
            "scenes": json.load(open(os.path.join(
                ROOT, "tests", "testdata", "tts", "tts_spec.json"),
                encoding="utf-8"))["scenes"],
        }
        spec_path = os.path.join(tmp, "tts_smoke.json")
        with open(spec_path, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False)

        out_spec = os.path.join(tmp, "tts_smoke.narrated.json")
        proc = run_tool([os.path.join(TOOLS, "hf_tts.py"), spec_path,
                         "-o", out_spec,
                         "--provider", "http", "--endpoint", endpoint])
        # 类型聚合：stats 场景无 text（文本在 value/description）也须有旁白
        assert "生成 3" in proc.stdout, proc.stdout

        narrated = json.load(open(out_spec, encoding="utf-8"))
        assert all(s.get("narration") for s in narrated["scenes"])
        voice_files = sorted(os.listdir(os.path.join(tmp, "voice")))
        # http provider 产 mp3（say 才是 wav）；文件名带文本短哈希——
        # 词变自动重生成，跨 spec 同目录不串音
        assert len(voice_files) == 3 and all(
            v.startswith(f"scene-{i}-") and v.endswith(".mp3")
            for i, v in enumerate(voice_files)), voice_files

        # 幂等复跑：narrated spec 全场景已有 narration → 零生成、全保留
        proc2 = run_tool([os.path.join(TOOLS, "hf_tts.py"), out_spec,
                          "--provider", "http", "--endpoint", endpoint])
        assert "生成 0" in proc2.stdout and "保留既有 3" in proc2.stdout, \
            proc2.stdout

        # 交付产物（参考管线第 2/4 步）：旁白全文 script.txt + 定位拼接
        # 的独立旁白轨 voice.m4a（与 narrated spec 同目录）
        assert os.path.isfile(os.path.join(tmp, "tts_smoke.script.txt")), \
            "script.txt 缺失"
        voice_track = os.path.join(tmp, "tts_smoke.voice.m4a")
        assert os.path.isfile(voice_track), "独立旁白轨缺失"
        probe = subprocess.run(["ffprobe", "-v", "error",
                                "-show_entries", "format=duration",
                                "-of", "csv=p=0", voice_track],
                               capture_output=True, text=True, timeout=60)
        assert float(probe.stdout.strip()) > 1.0, "旁白轨时长异常"

        # compose 拟合链：narrated spec → 契约 HTML（旁白发射 <audio>）
        html_path = os.path.join(tmp, "tts_smoke.html")
        run_tool([os.path.join(TOOLS, "hf_compose.py"), out_spec,
                  "-o", html_path])
        html = open(html_path, encoding="utf-8").read()
        assert html.startswith("<!DOCTYPE html>\n"
                               "<!-- xmedia-kit hf_compose"), "生成器注释缺失"
        assert "| schema:1.0" in html, "schema 版本锚点缺失"
        # 自定义模板协议：spec_dir/templates/<name>.json 注册 → 场景命中
        tpl_dir = os.path.join(tmp, "templates")
        os.makedirs(tpl_dir, exist_ok=True)
        with open(os.path.join(tpl_dir, "badge.json"), "w",
                  encoding="utf-8") as f:
            json.dump({"name": "badge",
                       "slots": ["title"],
                       "css": ".tpl-badge { color: #ffcc00; }",
                       "body": "<b class=\"tpl-badge\">{{title}}</b>"},
                      f, ensure_ascii=False)
        spec["scenes"].append({"type": "badge", "title": "徽标场景",
                               "duration": 1})
        spec_with_tpl = os.path.join(tmp, "with_tpl.json")
        json.dump(spec, open(spec_with_tpl, "w", encoding="utf-8"),
                  ensure_ascii=False)
        run_tool([os.path.join(TOOLS, "hf_compose.py"), spec_with_tpl,
                  "-o", os.path.join(tmp, "with_tpl.html")])
        tpl_html = open(os.path.join(tmp, "with_tpl.html"),
                        encoding="utf-8").read()
        assert "tpl-badge" in tpl_html and "徽标场景" in tpl_html, \
            "自定义模板未命中"
        assert '.tpl-badge { color: #ffcc00; }' in tpl_html, "模板 CSS 未注入"
        # schema 版本门：未知版本拒绝
        bad = os.path.join(tmp, "bad.json")
        json.dump({**spec, "schema_version": "9.9"},
                  open(bad, "w", encoding="utf-8"), ensure_ascii=False)
        p = subprocess.run([sys.executable,
                            os.path.join(TOOLS, "hf_compose.py"), bad],
                           capture_output=True, text=True)
        assert p.returncode == 1 and "不受支持" in p.stderr, \
            f"坏版本未被拒绝：{p.returncode} {p.stderr[-200:]}"
        # 3 旁白 + 3 SFX（SFX 目录解析兜底到仓库 assets/sfx；类型默认
        # whoosh/success/ding）
        assert html.count("<audio") == 6, f"SFX+旁白 <audio> 数量不符"
        assert 'data-time-unit="s"' in html, "秒制声明缺失"
        assert html.count("tl.fromTo") == 3, "GSAP 入场数量不符"

    server.shutdown()
    print("hf_tts_smoke: PASS")


if __name__ == "__main__":
    main()
