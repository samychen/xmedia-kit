#!/usr/bin/env python3
# hf_studio 冒烟（ctest hf_studio_smoke）：契约 HTML → Studio 注入。
# 验证 chrome/驱动注入、场景按钮、幂等原地更新、契约语义不变
# （hf_validate 在 studio 页仍 0 errors、场景 div 原样保留）。
import json
import os
import re
import shutil
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
        # 预览驱动协议：播放 API、父页命令、状态上报、嵌入模式让位
        assert "__mcStudioAPI" in html, "预览播放 API 缺失"
        assert "mcStudioCommand" in html, "父页控制命令缺失"
        assert "mcStudioState" in html, "播放状态上报缺失"
        assert "embed=1" in html and "EMBED" in html, "嵌入模式缺失"
        assert "mcStudioKey" in html, "嵌入模式快捷键转发缺失"
        # 预览音频引擎：契约页的 <audio> 需随播放头起播，并可静音/调音量
        for marker in ("syncAudio", "audioState", "setMuted", "setVolume",
                       "enableAudio", "data-mc-audio", "playbackRate",
                       "audioClock"):
            assert marker in html, f"预览音频驱动缺失：{marker}"
        # 防回归：播放中逐帧漂移校正会让每次 seek 重新缓冲 → 声音断续。
        assert "Math.abs(el.currentTime - want) > tol" not in html, \
            "播放中逐帧 seek 的漂移校正已回归（会导致声音断续）"
        # 防回归：播放头曾被音频时钟直接覆盖。新音轨起播 / 切换时它还在
        # 缓冲（currentTime=0），t 会被钉在 a.start 连续数帧，表现为
        # 「音效结束 / 下一段开始时垂直游标卡一下」。
        assert "function audioClock(ref)" in html, \
            "音频参考时钟须以当前时间为锚选择音轨"
        assert "el.currentTime <= 0.02" in html, \
            "未排除尚未推进（缓冲中）的音轨，播放头会被钉在素材起点"
        assert "SKEW_DEADBAND" in html and "SKEW_PULL" in html, \
            "音画时钟死区 / 微幅纠偏常量缺失"
        # 逐帧牵引会在音轨起播滞后时把步长压到 60%，游标在音效边界减速。
        assert "step * 0.6" not in html and "maxPull" not in html, \
            "旧的逐帧牵引已回归（音效起点播放头会连续减速）"
        assert "Math.max(0, clock)" not in html, \
            "音频时钟直接覆盖播放时间已回归（边界游标卡顿）"
        assert '=== "done"' in html, \
            "已播完音效的短路缺失（会每帧重新缓冲 / 重复起播）"
        # 音轨首次 play() 在真实浏览器里会同步初始化解码器 / 输出设备，
        # 留在 rAF 帧内会推迟状态上报，游标在 00:01.37 / 00:03.90 前几帧
        # 突然变卡（headless 空音频设备复现不出，靠静态契约守住）。
        assert "function warmAudio()" in html and "a.warming" in html, \
            "音轨预热缺失（首次 play 的解码开销仍在播放中途）"
        assert "audioPend" in html and "setTimeout(function" in html, \
            "播放态音频起播未移出 rAF 帧（会阻塞状态上报）"
        assert "状态上报放在最前" in html, \
            "状态上报未提前到画面 / 音频工作之前"
        assert "syncAudio(sec, playing, !!forceSeek);" not in html, \
            "播放态逐帧同步音频已回归"
        # Studio UI 静态契约：原生画布等比缩放 + 传输条 + 嵌入预览
        ui = open(os.path.join(TOOLS, "studio_ui.html"),
                  encoding="utf-8").read()
        assert 'id="pvframe"' in ui and "transform-origin: top left" in ui, \
            "预览画布未按原生尺寸 + transform 等比缩放"
        assert "/preview?embed=1" in ui and "__mcStudioAPI" in ui, \
            "UI 未接嵌入预览协议"
        for el in ('id="btn-play"', 'id="scrub"', 'id="zoom"',
                   'overlay safe-overlay', 'id="btn-loop"', 'id="tc"',
                   'id="btn-audio"', 'id="vol"', 'id="audio-hint"'):
            assert el in ui, f"预览区控件缺失：{el}"
        assert "renderAudioBtn" in ui and "unlockPreviewAudio" in ui and \
            "KeyM" in ui, "声音开关 / 自动播放解锁 / M 快捷键缺失"
        # 时间轴（NLE 风格）：工具栏 / 5 轨 / 可缩放画布 / 缩略图 / 波形
        for el in ('class="tl-toolbar"', 'id="tl-grid"', 'id="tl-scroll"',
                   'id="tl-zoom-in"', 'id="tl-snap"', 'id="tl-tc"',
                   'id="tl-zoom-label"'):
            assert el in ui, f"时间轴控件缺失：{el}"
        assert "function waveSVG(" in ui and "function stripHTML(" in ui, \
            "确定性波形 / filmstrip 生成缺失"
        assert "function zoomTimeline(" in ui and "tlPPS" in ui, \
            "时间轴按 px/s 缩放缺失"
        assert "v-caption" in ui and "trackHead" in ui and \
            "#playhead" in ui, "轨道头 / 播放头缺失"
        assert "tl-blk" not in ui and "tl-label" not in ui, \
            "旧版纯色块时间轴残留"
        # 字幕轨必须消费 spec.captions.segments（ASR / SRT 产物），只画
        # scenes[].text 会让「识别到 N 段字幕」之后字幕轨依然空白。
        assert "function capSegments(" in ui and \
            "captions.segments" in ui and "function capTrackSub(" in ui, \
            "字幕轨未读取全局字幕段（ASR 后轨道会空白）"
        # 播放头流畅度：CSS transition 在合成层插值推进，播放中 JS 不写
        # transform。Safari 同源 iframe 与父页共享主线程，音轨首次 play()
        # 的解码初始化会同步阻塞父页 rAF，任何 JS 逐帧驱动都会在音效起点
        # 前顿一下（录屏实测 00:01.37 前连续 13 帧停住）。
        assert "translate3d(" in ui and "will-change: transform" in ui, \
            "播放头未用 transform 推进（每帧改 left 会重排整条时间轴）"
        assert "ph.style.left" not in ui, "播放头仍在逐帧写 left"
        assert "startPhAnim" in ui and "stopPhAnim" in ui and \
            'ph.style.transition = "transform " + dur + "s linear"' in ui, \
            "合成层 CSS transition 播放头缺失（主线程阻塞会卡住游标）"
        assert "void ph.offsetWidth" in ui, \
            "启动过渡前未提交起点（会与上一次过渡串联）"
        assert "setPhTransform" in ui and "updatePlayheadMeta" in ui and \
            "followPlayhead" in ui, \
            "播放中的文本更新 / 立即定位 / 自动跟随未拆分"
        assert "lastTcHtml" in ui and "lastLabel" in ui, \
            "状态文本的每帧写入未做变化检测"
        assert "function phFrame(now)" in ui and \
            "requestAnimationFrame(phFrame)" in ui and \
            "updatePlayheadMeta(phVisual, total)" in ui, \
            "父页低频文本时钟缺失"
        phframe = ui.split("function phFrame(now)")[1].split("\n}")[0]
        assert "updatePlayhead(phVisual" not in phframe and \
            "ph.style.transform" not in phframe, \
            "phFrame 仍在逐帧写播放头 transform（会打断合成层过渡）"
        assert "phVisual" in ui and "phTarget" in ui, \
            "视觉时钟的当前值 / 目标值缺失"
        # 单条预混音预览：播放中途不再有元素首次 play()。Safari 的音轨
        # 解码初始化是同步的，会阻塞与父页共享的主线程，表现为第 2、3 个
        # 音效起点前游标连续数帧不动。注入模式（单文件）必须保持多轨引擎。
        sys.path.insert(0, TOOLS)
        import hf_studio as studio_mod
        mix_html = studio_mod.build_chrome(
            5.17, [], studio_mod.PREVIEW_MIX_URL)
        assert 'var MIX_URL = "/__xmedia_preview_mix__.wav"' in mix_html, \
            "混音音轨 URL 未注入预览驱动"
        assert 'if (MIX_URL) {' in mix_html and \
            'document.createElement("audio")' in mix_html and \
            'el.preload = "none"' in mix_html, \
            "单音轨分支缺失，或契约页原音轨未禁用预加载"
        assert 'var MIX_URL = ""' in html, \
            "注入模式（单文件 / 烘焙链路）不应启用混音 URL"
        # ffmpeg 可用时验证混音产物本身；不可用时服务会自动降级。
        if shutil.which("ffmpeg"):
            wav_src = os.path.join(ROOT, "tests", "testdata", "tts",
                                   "narr_sample_1.wav")
            mix_contract = os.path.join(tmp, "mix_smoke.html")
            with open(mix_contract, "w", encoding="utf-8") as f:
                f.write('<audio src="' + wav_src + '" data-start="0.25" '
                        'data-duration="1" data-volume="0.5"></audio>')
            mix_bytes = studio_mod.build_preview_mix(
                mix_contract, open(mix_contract, encoding="utf-8").read(), 2.0)
            assert mix_bytes and mix_bytes[:4] == b"RIFF", \
                "预混音 WAV 生成失败"

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
        # 混音可用 → 单音轨；不可用 → 优雅降级到契约页多轨引擎。
        if server.preview_mix():
            assert 'var MIX_URL = "/__xmedia_preview_mix__.wav"' in prev, \
                "服务预览未启用单条混音音轨"
            req = urllib.request.Request(
                base + studio_mod.PREVIEW_MIX_URL,
                headers={"Range": "bytes=0-1023"})
            with urllib.request.urlopen(req, timeout=30) as r:
                assert r.status == 206 and \
                    r.headers.get("Content-Type", "").startswith("audio/wav") and \
                    r.read(4) == b"RIFF", \
                    "预览混音路由未支持 Range（Safari 音频 seek 会失败）"
        else:
            assert 'var MIX_URL = ""' in prev, \
                "混音不可用时未降级到多轨引擎"

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
        assert studio_mod.ROOT_ASSET_PREFIX not in contract_now, \
            "HTTP 路径改写污染了契约文件（会破坏 file:// 烘焙）"

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

        # 预览页：音频引擎已注入，且每一条音轨（旁白 + 根目录 SFX）
        # 都能经 HTTP 取到——绝对路径不能再 404。
        with urllib.request.urlopen(base + "/preview", timeout=30) as r:
            prev = r.read().decode("utf-8")
        for marker in ("syncAudio", "audioState", "setMuted",
                       "enableAudio", "data-mc-audio", "audioClock"):
            assert marker in prev, f"预览页音频驱动缺失：{marker}"
        assert "Math.abs(el.currentTime - want) > tol" not in prev, \
            "预览页残留播放中逐帧 seek 的漂移校正"
        audio_srcs = re.findall(r'<audio[^>]*\bsrc="([^"]+)"', prev)
        assert audio_srcs, "预览页没有音轨"
        for src in audio_srcs:
            url = src if src.startswith("http") else base + "/" + src.lstrip("/")
            with urllib.request.urlopen(url, timeout=30) as r:
                assert r.status == 200 and \
                    int(r.headers.get("Content-Length", 0)) > 0, \
                    f"预览音轨不可达：{src}"
        assert any(studio_mod.ROOT_ASSET_PREFIX in s for s in audio_srcs), \
            "内置 SFX 的绝对路径未映射为可服务 URL"

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

        # AI 能力静态契约：Agent / ASR / 导入端点与 Ask AI 面板不可回退
        srv_src = open(os.path.join(TOOLS, "hf_studio.py"),
                       encoding="utf-8").read()
        ui_src = open(os.path.join(TOOLS, "studio_ui.html"),
                      encoding="utf-8").read()
        for token in ("/api/agent", "/api/asr", "/api/import",
                      "_run_agent", "_run_asr", "_run_import",
                      "transcribe_with_fallback", "hf_agent.validate_spec"):
            assert token in srv_src, f"Studio 服务缺少 {token}"
        for token in ('id="ask-fab"', 'id="ask-panel"', 'id="ask-quick"',
                      "/api/agent", "function askSend", "KeyK"):
            assert token in ui_src, f"Studio UI 缺少 {token}"
        # 导入素材必须有独立按钮入口，且外部素材在 HTTP 预览下不能 404。
        for token in ("/api/import/upload", "_save_uploads",
                      "EXTERNAL_ASSET_PREFIX", "MAX_UPLOAD_BYTES"):
            assert token in srv_src, f"Studio 服务缺少 {token}"
        for token in ('id="btn-import"', 'id="imp-panel"', 'id="imp-dir"',
                      'id="imp-go"', "function impRun", "/api/import/upload"):
            assert token in ui_src, f"Studio UI 缺少 {token}"

    httpd.shutdown()
    print("hf_studio_smoke: PASS")


if __name__ == "__main__":
    main()
