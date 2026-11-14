#!/usr/bin/env python3
# hf_studio 冒烟（ctest hf_studio_smoke）：契约 HTML → Studio 注入。
# 验证 chrome/驱动注入、场景按钮、幂等原地更新、契约语义不变
# （hf_validate 在 studio 页仍 0 errors、场景 div 原样保留）。
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

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

    # ---- 服务模式闭环：state / recompose（写回契约文件）/ TTS / bake ----
    wav = open(os.path.join(ROOT, "tests", "testdata", "tts",
                            "narr_sample_1.wav"), "rb").read()

    class MockTTS(BaseHTTPRequestHandler):
        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", 0)))
            self.send_response(200)
            self.send_header("Content-Length", str(len(wav)))
            self.end_headers()
            self.wfile.write(wav)

        def log_message(self, *args):
            pass

    srv = HTTPServer(("127.0.0.1", 0), MockTTS)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    endpoint = f"http://127.0.0.1:{srv.server_address[1]}/tts"

    sys.path.insert(0, TOOLS)
    import hf_studio as studio_mod

    with tempfile.TemporaryDirectory(prefix="hf_studio_srv_") as tmp:
        spec = {
            "composition": {"id": "srv-smoke", "fps": 30},
            "tts": {"provider": "http", "endpoint": endpoint},
            "scenes": [
                {"type": "title", "text": "服务冒烟", "duration": 2,
                 "bg": "#1a1a2e"},
                {"type": "outro", "text": "结束", "duration": 1.5,
                 "bg": "#0f3460"},
            ],
        }
        spec_path = os.path.join(tmp, "srv_smoke.json")
        with open(spec_path, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False)
        contract = os.path.join(tmp, "srv_smoke.html")
        run_tool([os.path.join(TOOLS, "hf_compose.py"), spec_path,
                  "-o", contract])

        server = studio_mod.StudioServer(contract, spec_path, 0, False)
        httpd = studio_mod.ThreadingHTTPServer(("127.0.0.1", 0),
                                               studio_mod.make_handler(server))
        threading.Thread(target=httpd.serve_forever, daemon=True).start()
        base = f"http://127.0.0.1:{httpd.server_address[1]}"

        with urllib.request.urlopen(base + "/api/state", timeout=30) as r:
            st = json.load(r)
        assert st["spec_path"] == spec_path and st["errors"] == 0, st["findings"]
        assert st["width"] == 1080 and st["height"] == 1920, st

        with urllib.request.urlopen(base + "/preview", timeout=30) as r:
            prev = r.read().decode("utf-8")
        assert studio_mod.MARK_BEGIN in prev and "__mcStudio" in prev, \
            "预览页未注入驱动"

        # 重组：改文字 → 契约文件写回 + 校验结果随响应
        st["spec"]["scenes"][0]["text"] = "改词后的标题"
        req = urllib.request.Request(
            base + "/api/recompose",
            data=json.dumps({"spec": st["spec"]}).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=60) as r:
            rc = json.load(r)
        assert rc["ok"] and rc["sha"], rc
        contract_now = open(contract, encoding="utf-8").read()
        assert "改词后的标题" in contract_now, "重组未写回契约文件"

        # 音频路由：spec 目录下的 narration 文件可经服务读取
        # recompose 前的 spec 无 narration——由 TTS 步骤补齐后再验
        req = urllib.request.Request(
            base + "/api/tts",
            data=json.dumps({"spec": st["spec"]}).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=300) as r:
            tts_result = json.load(r)
        assert tts_result["generated"] >= 2, tts_result
        narr = tts_result["spec"]["scenes"][0]["narration"]
        assert narr and "scene-0-" in narr, narr
        with urllib.request.urlopen(base + "/" + narr, timeout=30) as r:
            assert r.status == 200 and int(r.headers.get("Content-Length", 0)) > 0, \
                "音频路由失败"

        # 烘焙闭环（参数由 spec 自动推导）：rc 0 + 产物存在
        req = urllib.request.Request(base + "/api/bake", data=b"{}",
                                     headers={"Content-Type": "application/json"},
                                     method="POST")
        with urllib.request.urlopen(req, timeout=60) as r:
            job_id = json.load(r)["job"]
        for _ in range(120):
            with urllib.request.urlopen(base + f"/api/job?id={job_id}",
                                        timeout=30) as r:
                j = json.load(r)
            if not j["running"]:
                break
            time.sleep(0.5)
        assert j["rc"] == 0, f"烘焙失败：{j['tail']}"
        assert os.path.isfile(j["out"]), "烘焙产物缺失"

        # TTS 词变重生成：改文字后 narration 的文本哈希应更新
        st["spec"]["scenes"][0]["text"] = "词变重生成验证"
        req = urllib.request.Request(
            base + "/api/tts", data=json.dumps({"spec": st["spec"]}).encode(),
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=300) as r:
            r2 = json.load(r)
        narr2 = r2["spec"]["scenes"][0]["narration"]
        assert narr2 != narr and "scene-0-" in narr2, (narr, narr2)

    httpd.shutdown()
    print("hf_studio_smoke: PASS")


if __name__ == "__main__":
    main()
