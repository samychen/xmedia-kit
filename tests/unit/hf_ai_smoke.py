#!/usr/bin/env python3
# hf_ai 冒烟（ctest hf_ai_smoke）：ASR 字幕 / 素材自动导入 / Agent 规则 /
# MCP stdio / Ask AI HTTP 闭环 / 分段字幕契约。
# 只用标准库 + ffmpeg/ffprobe，不依赖 8120 常驻服务（临时端口自起自停）。
import glob
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOLS = os.path.join(ROOT, "tools")
sys.path.insert(0, TOOLS)


def run(args, timeout=180):
    proc = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
    assert proc.returncode == 0, (
        f"exit {proc.returncode}: {' '.join(args)}\n{proc.stderr[-600:]}")
    return proc


def make_clips(tmp):
    for name, dur in (("a_clip.mp4", 1.5), ("b_clip.mp4", 2.0)):
        run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi",
             "-i", f"testsrc=duration={dur}:size=320x180:rate=30",
             "-f", "lavfi", "-i", f"sine=frequency=440:duration={dur}",
             "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac",
             "-shortest", os.path.join(tmp, name)])
    srt = os.path.join(tmp, "cap.srt")
    with open(srt, "w", encoding="utf-8") as f:
        f.write("1\n00:00:00,300 --> 00:00:01,100\n第一段\n\n"
                "2\n00:00:01,800 --> 00:00:03,000\n第二段\n")
    return srt


def check_asr():
    import hf_asr as A
    assert A.parse_timestamp("00:00:01,250") == 1.25
    assert A.parse_timestamp("00:01.500") == 1.5
    segs = A.parse_srt("WEBVTT\n\n1\n00:00:00.500 --> 00:00:01.500\n"
                       "Hello <b>world</b>\n\n2\n00:00:02.000 --> "
                       "00:00:03.000\nSecond\n")
    assert segs == [
        {"start": 0.5, "end": 1.5, "text": "Hello world"},
        {"start": 2.0, "end": 3.0, "text": "Second"}], segs
    whisper = {"transcription": [
        {"timestamps": {"from": "00:00:00,000", "to": "00:00:02,000"},
         "text": " 测试 "}]}
    assert A.parse_whisper_json(whisper) == [
        {"start": 0.0, "end": 2.0, "text": "测试"}]
    assert A.parse_whisper_json({"segments": [
        {"start": 1, "end": 2, "text": "api"}]}) == [
        {"start": 1.0, "end": 2.0, "text": "api"}]
    clipped = A.segments_for_scene(segs, 1.0, 1.5)
    assert clipped[0]["start"] == 0.0 and clipped[0]["end"] == 0.5
    assert clipped[1]["start"] == 1.0
    dup = A.normalize_segments(segs + segs)
    assert len(dup) == 2, "重复段未去重"
    print("  ASR 解析 / 裁剪 / 去重 OK")


def check_import(tmp, srt):
    import hf_import as I
    import hf_compose as C
    import hf_validate as V
    spec, notes = I.build_spec(tmp, tmp, captions_file=srt, endcard=True)
    assert len(spec["scenes"]) == 3
    assert [s["type"] for s in spec["scenes"]] == [
        "text", "text", "outro-brand"]
    assert [s["duration"] for s in spec["scenes"]] == [1.5, 2.0, 2.5]
    assert spec["scenes"][0]["video"] == "a_clip.mp4"
    assert spec["transitions"]["type"] == "none"
    assert spec["sfx"]["enabled"] is False
    assert len(spec["captions"]["segments"]) == 2
    spec2, _ = I.build_spec(tmp, tmp, captions_file=srt, endcard=True)
    assert json.dumps(spec, sort_keys=True) == json.dumps(spec2, sort_keys=True), \
        "导入不确定（同目录两次结果不同）"
    # 单文件导入：与「只含一个素材的目录」等价，spec 目录回落为文件所在目录
    single, notes = I.build_spec(os.path.join(tmp, "a_clip.mp4"))
    assert len(single["scenes"]) == 1, single["scenes"]
    assert single["scenes"][0]["duration"] == 1.5
    assert single["scenes"][0]["video"] == "a_clip.mp4", single["scenes"][0]
    assert notes[0].startswith("导入 1 个场景"), notes
    try:
        I.build_spec(srt)
        raise AssertionError("非音视频文件不应被当成素材导入")
    except AssertionError:
        raise
    except Exception as e:
        assert "不支持该文件类型" in str(e), e
    print("  单文件导入 / 类型校验 OK")
    C._spec_dir, C._gsap_src = tmp, "/* gsap stub */"
    html = C.compose(spec)
    findings, errors, _w, _i = V.validate(html)
    assert errors == 0, findings
    # 全局字幕按场景窗口自动分配：段1→scene-0，段2→scene-1
    assert 'id="cap-0-0"' in html and 'id="cap-1-0"' in html
    assert "cap-2-0" not in html
    print("  素材导入 / 确定性 / 跨场景字幕 OK")
    return spec


def check_compose_captions(tmp):
    import hf_compose as C
    import hf_validate as V
    C._spec_dir, C._gsap_src = tmp, "/* gsap stub */"
    spec = {"schema_version": "1.0", "composition": {"fps": 30},
            "scenes": [{"type": "text", "text": "画面", "duration": 4}],
            "captions": {"enabled": True, "segments": [
                {"start": 0.5, "end": 1.5, "text": "第一句"},
                {"start": 2.0, "end": 3.0, "text": "第二句"}]},
            "sfx": {"enabled": False}}
    h1, h2 = C.compose(spec), C.compose(spec)
    assert h1 == h2, "分段字幕破坏确定性"
    # 每段 3 条 set（初始隐藏 / 起点点亮 / 终点熄灭），零时长才无边界偏差
    assert h1.count('tl.set("#cap-') == 6, h1.count('tl.set("#cap-')
    assert ".scene-caption.seg { opacity: 0; }" in h1
    assert 'tl.fromTo("#cap-' not in h1, "非零时长字幕 tween 会错边界帧"
    f, e, _w, _i = V.validate(h1)
    assert e == 0
    off = dict(spec, captions={"enabled": False})
    assert "scene-caption seg" not in C.compose(off), "关闭字幕仍渲染分段"
    # 场景级相对字幕
    local = {"schema_version": "1.0",
             "scenes": [{"type": "text", "duration": 3,
                         "captions": [{"start": 0, "end": 1, "text": "局部"}]}]}
    assert "cap-0-0" in C.compose(local)
    print("  分段字幕 GSAP set / 开关 / 场景级 OK")


def check_agent():
    import hf_agent as A
    spec = {"schema_version": "1.0",
            "composition": {"format": "16:9", "fps": 30},
            "scenes": [{"type": "text", "text": "A", "duration": 3},
                       {"type": "text", "text": "B", "duration": 2}]}
    r = A.handle("加一个片尾卡", spec)
    assert len(r["spec"]["scenes"]) == 3 and \
        r["spec"]["scenes"][-1]["type"] == "outro-brand"
    assert A.handle("改成竖屏", spec)["spec"]["composition"]["format"] == "9:16"
    assert A.handle("第2个场景文字改成 你好", spec)["spec"]["scenes"][1]["text"] == "你好"
    assert len(A.handle("删除第1个场景", spec)["spec"]["scenes"]) == 1
    assert A.handle("关闭字幕", spec)["spec"]["captions"]["enabled"] is False
    assert A.handle("生成旁白", spec)["action"] == "tts"
    assert A.handle("导出 MP4", spec)["action"] == "bake"
    assert A.handle("烘焙 MP4", spec)["action"] == "bake"  # 旧说法仍兼容
    imp = A.handle("导入 ~/Downloads 并加字幕和片尾卡", spec)
    assert imp["action"] == "import" and imp["args"]["asr"] and \
        imp["args"]["endcard"]
    assert A.handle("随便说点什么", spec)["mode"] == "help"
    for r in (A.handle(m, spec) for m in
              ("加片尾卡", "改成竖屏", "关闭字幕", "第1个场景时长 4 秒")):
        if "spec" in r:
            A.validate_spec(r["spec"])
    for bad in ({"scenes": []},
                {"scenes": [{"type": "not-a-type", "duration": 1}]},
                {"scenes": [{"type": "text", "duration": -1}]},
                {"scenes": [{"type": "text", "duration": 2,
                             "video": "https://evil/x.mp4"}]}):
        try:
            A.validate_spec(bad)
        except A.AgentRejected:
            pass
        else:
            raise AssertionError(f"非法 spec 未被拒绝：{bad}")
    print("  Agent 规则 / 硬门校验 OK")


def check_mcp():
    reqs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
        {"jsonrpc": "2.0", "method": "notifications/initialized"},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
        {"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {
            "name": "xmedia_help", "arguments": {}}},
        {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {
            "name": "xmedia_validate", "arguments": {"spec": {
                "schema_version": "1.0",
                "scenes": [{"type": "text", "duration": 2}]}}}},
    ]
    payload = "\n".join(json.dumps(r) for r in reqs) + "\n"
    proc = subprocess.run([sys.executable, os.path.join(TOOLS,
                          "xmedia_mcp.py")], input=payload,
                          capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, proc.stderr[-400:]
    lines = [json.loads(x) for x in proc.stdout.strip().splitlines() if x.strip()]
    assert [x["id"] for x in lines] == [1, 2, 3, 4], "通知不应产生响应"
    assert lines[0]["result"]["protocolVersion"] == "2024-11-05"
    names = {t["name"] for t in lines[1]["result"]["tools"]}
    assert {"xmedia_state", "xmedia_recompose", "xmedia_import",
            "xmedia_asr", "xmedia_agent", "xmedia_validate"} <= names
    assert '"ok": true' in lines[3]["result"]["content"][0]["text"]
    print("  MCP stdio 协议 / 11 tools / 本地校验 OK")


def check_cli_and_ui():
    out = run([sys.executable, os.path.join(TOOLS, "xmedia.py"),
               "--help"]).stdout
    for cmd in ("import", "asr", "agent", "mcp"):
        assert cmd in out, f"CLI 缺少 {cmd} 子命令"
    ui = open(os.path.join(TOOLS, "studio_ui.html"), encoding="utf-8").read()
    # M5 ESM：UI 逻辑迁到 tools/web/src 分片，打包后函数名被 minify 改名
    # ——文本断言与函数抽取（check_split_math/extract_fn）都指向
    # 「产物 + 分片源」拼接文本（分片源是唯一事实）。
    for f in (glob.glob(os.path.join(TOOLS, "web", "src", "**", "*.js"),
                        recursive=True)
              + glob.glob(os.path.join(TOOLS, "web", "src", "**", "*.html"),
                          recursive=True)):
        ui += open(f, encoding="utf-8").read() + "\n"
    for marker in ('id="ask-fab"', 'id="ask-panel"', 'id="ask-msgs"',
                   'id="ask-quick"', 'id="ask-input"', '/api/agent',
                   'function askSend', 'KeyK',
                   # 导入素材按钮入口：不能只留 Ask AI 里的自然语言指令
                   'id="btn-import"', 'id="imp-panel"', 'id="imp-dir"',
                   'id="imp-picker"', 'id="imp-drop"', 'id="imp-go"',
                   '/api/import/upload', 'function impUpload',
                   'function impRun',
                   # 时间轴片段切割：按钮 + 快捷键 + 源内起点（video_from）
                   'id="tl-split"', 'function splitScene',
                   'function splitSceneData', 'function splitAtPlayhead',
                   'code === "KeyS"', 'data-media-start', 'video_from',
                   # 转场 / 入场动画改完立即生效（不必再点「应用到场景」）
                   '$("insp-trans").onchange', '$("insp-anim").onchange',
                   'function sameSourceSeam',
                   'value="none">none（硬切进入）'):
        assert marker in ui, f"Studio UI 缺少 {marker}"
    studio = open(os.path.join(TOOLS, "hf_studio.py"), encoding="utf-8").read()
    for marker in ('/api/import/upload', '_save_uploads',
                   'EXTERNAL_ASSET_PREFIX', 'MAX_UPLOAD_BYTES',
                   # 预览驱动与缩略图都按源内起点对位（切割后段从该处取帧）
                   'data-media-start', 'start=s0, dur=d0'):
        assert marker in studio, f"Studio 服务缺少 {marker}"
    check_split_math(ui)
    print("  CLI 注册 / Ask AI + 导入面板静态契约 OK")


def extract_fn(src, name):
    """从 UI 脚本里抠出一个顶层函数文本（括号配平），交给 node 单独跑。"""
    i = src.index("function " + name)
    j = src.index("{", i)
    depth = 0
    for k in range(j, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[i:k + 1]
    raise AssertionError(f"函数 {name} 括号不配平")


def check_split_math(ui):
    """切割是纯数据变换：拿真函数跑 node，验证时长分割与源内起点累加。"""
    node = shutil.which("node")
    if not node:
        print("  （跳过切割数学断言：未安装 node）")
        return
    fn = extract_fn(ui, "splitSceneData")
    script = fn + r"""
function eq(a, b, m) {
  if (Math.abs(a - b) > 1e-6) throw new Error(m + "：" + a + " != " + b);
}
var a = {duration: 2, video: "kpl.mp4"};
var ta = splitSceneData(a, 0.75, 2);
eq(a.duration, 0.75, "前段时长");
eq(ta.duration, 1.25, "后段时长");
eq(ta.video_from, 0.75, "缺省源内起点应为刀口");
var b = {duration: 3, video: "kpl.mp4", video_from: 1.5};
var tb = splitSceneData(b, 1.0, 3);
eq(b.duration, 1.0, "前段时长2");
eq(tb.duration, 2.0, "后段时长2");
eq(tb.video_from, 2.5, "源内起点应累加刀口");
var c = {duration: 2, text: "hi"};
var tc = splitSceneData(c, 0.5, 2);
if ("video_from" in tc) throw new Error("非视频场景不该带 video_from");
if (tc.text !== "hi") throw new Error("文字应继承到后段");
// 接缝必须是硬切：两段本是同一画面的连续帧，继承来的入场/转场会在
// 刀口再淡入一次（看起来像转场没生效），还会让总时长被重叠吃掉。
var d = {duration: 2, video: "kpl.mp4", transition: "fade",
         animation: "fade_in"};
var td = splitSceneData(d, 1.0, 2);
if (d.transition !== "fade") throw new Error("前段应保留原转场");
if (td.transition !== "none") throw new Error("接缝转场必须为 none");
if (td.animation !== "none") throw new Error("接缝不该再淡入");
console.log("ok");
"""
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        run([node, path])
    finally:
        os.unlink(path)
    print("  切割数学（时长分割 / video_from 累加 / 接缝硬切）OK")


def check_http(tmp, spec_path, contract):
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    srv = subprocess.Popen(
        [sys.executable, os.path.join(TOOLS, "hf_studio.py"), contract,
         "--serve", "--spec", spec_path, "--port", str(port)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    base = f"http://127.0.0.1:{port}"
    try:
        for _ in range(80):
            try:
                urllib.request.urlopen(base + "/api/state", timeout=2).read()
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise AssertionError("临时 Studio 服务未就绪")

        def get(path):
            return json.load(urllib.request.urlopen(base + path, timeout=60))

        def post(path, body):
            req = urllib.request.Request(
                base + path, data=json.dumps(body, ensure_ascii=False).encode(),
                headers={"Content-Type": "application/json"}, method="POST")
            return json.load(urllib.request.urlopen(req, timeout=180))

        st = get("/api/state")
        assert st["agent"]["mcp"] is True and "llm" in st["agent"]
        assert st["errors"] == 0
        before = len(st["spec"]["scenes"])
        r = post("/api/agent", {"message": "加一个片尾卡"})
        assert r["composed"]["ok"] is True
        assert len(r["spec"]["scenes"]) == before + 1
        assert get("/api/state")["errors"] == 0
        r = post("/api/agent", {"message": "改成竖屏"})
        assert r["spec"]["composition"]["format"] == "9:16"
        r = post("/api/asr", {"path": os.path.join(tmp, "cap.srt"),
                              "provider": "srt"})
        assert len(r["segments"]) == 2
        # 默认「替换」：导入 2 个视频 + 1 片尾卡后，时间轴只剩本次的 3 个场景，
        # 上一次导入的内容不会残留到新素材前面。
        r = post("/api/import", {"dir": tmp,
                                 "captions": os.path.join(tmp, "cap.srt"),
                                 "endcard": True})
        assert r["composed"]["ok"] is True
        assert r["mode"] == "replace", r.get("mode")
        assert len(r["spec"]["scenes"]) == 3, len(r["spec"]["scenes"])
        assert r["can_undo"] is True, "替换后必须留下可撤销快照"
        # 显式 append=true 才保留原有内容（3 + 3）
        r = post("/api/import", {"dir": tmp,
                                 "captions": os.path.join(tmp, "cap.srt"),
                                 "endcard": True, "append": True})
        assert r["mode"] == "append" and \
            len(r["spec"]["scenes"]) == 6, (r.get("mode"),
                                            len(r["spec"]["scenes"]))
        # 撤销导入：回滚到追加之前
        r = post("/api/import/undo", {})
        assert len(r["spec"]["scenes"]) == 3, len(r["spec"]["scenes"])
        # 零依赖降级：无 whisper 时用唯一一份 SRT 兜底
        r = post("/api/agent", {"message": "自动字幕"})
        assert r["action"] == "asr" and r["asr"]["ok"] is True, \
            r.get("asr")
        assert get("/api/state")["errors"] == 0
        print("  HTTP /api/state|agent|asr|import 闭环 OK"
              "（ASR 走 SRT 降级）")

        # ---- Studio 导入面板：文件选择器上传 + 外部绝对路径素材只读映射 ----
        def multipart(names):
            boundary = "----xmediaAiSmoke"
            parts = []
            for name in names:
                with open(os.path.join(tmp, name), "rb") as f:
                    data = f.read()
                parts.append(
                    (f"--{boundary}\r\n"
                     f'Content-Disposition: form-data; name="file"; '
                     f'filename="{name}"\r\n'
                     "Content-Type: application/octet-stream\r\n\r\n"
                     ).encode() + data + b"\r\n")
            parts.append(f"--{boundary}--\r\n".encode())
            return b"".join(parts), f"multipart/form-data; boundary={boundary}"

        body, ctype = multipart(["a_clip.mp4", "b_clip.mp4"])
        req = urllib.request.Request(
            base + "/api/import/upload", data=body,
            headers={"Content-Type": ctype}, method="POST")
        up = json.load(urllib.request.urlopen(req, timeout=180))
        assert up["count"] == 2 and up["dir"].startswith(tmp), up
        r = post("/api/import", {"dir": up["dir"], "format": "9:16",
                                 "replace": True})
        assert r["composed"]["ok"] is True and len(r["spec"]["scenes"]) == 2
        assert r["spec"]["composition"]["format"] == "9:16", r["spec"]
        # 上传目录在 spec 目录内 → 预览用相对路径即可取到素材
        prev = urllib.request.urlopen(base + "/preview", timeout=60).read().decode()
        for src in re.findall(r'<video[^>]*\bsrc="([^"]+)"', prev):
            with urllib.request.urlopen(base + "/" + src.lstrip("/"),
                                        timeout=60) as resp:
                assert resp.status == 200 and len(resp.read()) > 1000, src

        # 外部目录（spec 目录之外）→ 绝对路径，必须经只读映射才不 404
        with tempfile.TemporaryDirectory(prefix="hf_ai_external_") as ext:
            for name in ("a_clip.mp4", "b_clip.mp4"):
                shutil.copyfile(os.path.join(tmp, name),
                                os.path.join(ext, name))
            r = post("/api/import", {"dir": ext, "endcard": True,
                                    "replace": True})
            assert r["composed"]["ok"] is True
            assert os.path.isabs(r["spec"]["scenes"][0]["video"]), r["spec"]
            prev = urllib.request.urlopen(base + "/preview",
                                          timeout=60).read().decode()
            videos = re.findall(r'<video[^>]*\bsrc="([^"]+)"', prev)
            assert videos and all("/__xmedia_media__/" in v for v in videos), \
                f"外部素材未映射为可服务 URL：{videos}"
            for v in videos:
                with urllib.request.urlopen(base + v, timeout=60) as resp:
                    assert resp.status == 200 and len(resp.read()) > 1000, v
            st = get("/api/state")
            assert st["import"]["last_dir"] == ext and \
                st["import"]["upload_max_mb"] >= 128, st["import"]
        print("  HTTP 导入面板：上传 / 外部素材只读映射 / state.import OK")
    finally:
        srv.terminate()
        try:
            srv.wait(timeout=8)
        except subprocess.TimeoutExpired:
            srv.kill()


def check_transition_window(tmp):
    """场景级转场的窗口：全局 transitions.duration=0 时必须仍开窗。

    全局段是「默认值」，可导入/新建的 spec 往往写着 {type:none,duration:0}；
    这时面板上给某一段选 slide / wipe，类型写进去了、重叠窗却是 0，画面
    完全不动——用户看到的就是「转场没反应」。
    """
    import hf_compose as C
    C._spec_dir, C._gsap_src = tmp, "/* gsap stub */"
    base = {"schema_version": "1.0",
            "composition": {"format": "16:9", "fps": 30},
            "transitions": {"type": "none", "duration": 0},
            "sfx": {"enabled": False}}

    def html_of(scenes):
        return C.compose(dict(base, scenes=scenes), spec_name="t.json")

    plain = html_of([{"type": "text", "text": "A", "duration": 2.0},
                     {"type": "text", "text": "B", "duration": 2.0}])
    assert 'tl.to("#scene-0"' not in plain, "没声明转场的场景不该出现出场动画"
    assert 'data-duration="4.0"' in plain, "无转场时总时长应为两段之和"

    slide = html_of([{"type": "text", "text": "A", "duration": 2.0},
                     {"type": "text", "text": "B", "duration": 2.0,
                      "transition": "slide-left"}])
    assert 'tl.to("#scene-0", {x: -120' in slide, \
        "场景级转场在全局 duration=0 时也必须开窗"
    assert 'data-duration="3.5"' in slide, "开了 0.5s 转场窗后总时长应变 3.5"

    # animation:"none" + transition：进入侧必须补回淡入，否则瞬间盖满，
    # 前一场景的出场动画全被遮住（看着还是硬切）。
    both = html_of([{"type": "text", "text": "A", "duration": 2.0},
                    {"type": "text", "text": "B", "duration": 2.0,
                     "transition": "fade", "animation": "none"}])
    assert 'tl.fromTo("#scene-1"' in both, "有转场时入场动画应回落默认淡入"
    print("  转场窗口（全局 0 仍开窗 / animation none 让位）OK")


def main():
    with tempfile.TemporaryDirectory(prefix="hf_ai_smoke_") as tmp:
        check_asr()
        srt = make_clips(tmp)
        spec = check_import(tmp, srt)
        check_compose_captions(tmp)
        check_transition_window(tmp)
        check_agent()
        check_mcp()
        check_cli_and_ui()
        spec_path = os.path.join(tmp, "spec.json")
        contract = os.path.join(tmp, "contract.html")
        # HTTP 用一份不带片尾卡的 spec，才能验证「加一个片尾卡」真的改了 spec
        http_spec = {
            "schema_version": "1.0",
            "composition": {"format": "16:9", "fps": 30},
            "transitions": {"type": "none", "duration": 0},
            "sfx": {"enabled": False},
            "scenes": [
                {"type": "text", "text": "A", "duration": 1.5,
                 "video": "a_clip.mp4"},
                {"type": "text", "text": "B", "duration": 2.0,
                 "video": "b_clip.mp4"}],
        }
        with open(spec_path, "w", encoding="utf-8") as f:
            json.dump(http_spec, f, ensure_ascii=False, indent=2)
        run([sys.executable, os.path.join(TOOLS, "hf_compose.py"),
             spec_path, "-o", contract])
        check_http(tmp, spec_path, contract)
    print("hf_ai_smoke: ALL PASS")


if __name__ == "__main__":
    main()
