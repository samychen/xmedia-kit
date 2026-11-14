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
        # http provider 产 mp3（say 才是 wav）——扩展名随 provider
        assert voice_files == ["scene-0.mp3", "scene-1.mp3", "scene-2.mp3"], \
            voice_files

        # 幂等复跑：narrated spec 全场景已有 narration → 零生成、全保留
        proc2 = run_tool([os.path.join(TOOLS, "hf_tts.py"), out_spec,
                          "--provider", "http", "--endpoint", endpoint])
        assert "生成 0" in proc2.stdout and "保留既有 3" in proc2.stdout, \
            proc2.stdout

        # compose 拟合链：narrated spec → 契约 HTML（旁白发射 <audio>）
        html_path = os.path.join(tmp, "tts_smoke.html")
        run_tool([os.path.join(TOOLS, "hf_compose.py"), out_spec,
                  "-o", html_path])
        html = open(html_path, encoding="utf-8").read()
        assert html.startswith("<!DOCTYPE html>\n"
                               "<!-- xmedia-kit hf_compose"), "生成器注释缺失"
        assert html.count("<audio") == 3, "旁白 <audio> 数量不符"
        assert 'data-time-unit="s"' in html, "秒制声明缺失"
        assert html.count("tl.fromTo") == 3, "GSAP 入场数量不符"

    server.shutdown()
    print("hf_tts_smoke: PASS")


if __name__ == "__main__":
    main()
