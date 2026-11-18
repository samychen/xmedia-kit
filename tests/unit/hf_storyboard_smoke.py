#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""HF Studio app 对齐新能力的自动化断言（2026-11-18）。

覆盖四块此前只有人工浏览器验证的能力：
  1. `xmedia storyboard <arc>`：5 个商业叙事 Arc 全部产出合法骨架
     （spec.storyboard 段 + 叙事词汇 + compose/validate 0 errors）；
  2. Home 项目管理（--home）：项目管理页、/api/projects、
     /api/project/create（骨架 → contract → 校验 0 errors）→ 项目列表；
  3. reference 对话链：Ask AI「参考 <路径> 讲 X」→ hf_reference 切镜头
     → 同节奏 spec 写回（场景数=镜头数）；
  4. 双屏同钟对照：/api/reference/video 登记 → /api/state 的 ref_video
     → asset 路由 Range/206（mp4 moov 尾部 seek 依赖）；
  5. /api/sb-frames 的 html_bake 缺失降级（400 + 提示，不 500）。

环境坑（与 hf_studio._ff_env 同根因）：托管运行时注入的 LD_LIBRARY_PATH
与系统 ffmpeg/ffprobe 插件依赖冲突——进程入口剥掉，子进程全部继承干净
环境（hf_ai_smoke 的教训：只净化子进程管不到进程内直调）。
"""

import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.request

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "tools")
sys.path.insert(0, TOOLS)

os.environ.pop("LD_LIBRARY_PATH", None)

XMedia = os.path.join(TOOLS, "xmedia.py")
HfStudio = os.path.join(TOOLS, "hf_studio.py")


def run(args, timeout=180):
    proc = subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout)
    assert proc.returncode == 0, (
        f"exit {proc.returncode}: {' '.join(map(str, args))}\n"
        f"{proc.stderr[-600:]}")
    return proc


def http(method, url, body=None, headers=None, timeout=30):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def jbody(raw):
    return json.loads(raw.decode("utf-8"))


def _make_ref_video(path):
    """三色硬切测试片（white/black/red，2.2/3.1/1.8s）——场景检测的
    对照素材；每次 smoke 自产，不依赖外部 fixture。"""
    run(["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", "color=c=white:s=320x240:d=2.2",
         "-f", "lavfi", "-i", "color=c=black:s=320x240:d=3.1",
         "-f", "lavfi", "-i", "color=c=0xff3333:s=320x240:d=1.8",
         "-filter_complex", "[0:v][1:v][2:v]concat=n=3:v=1", path])


def main():
    tmp = tempfile.mkdtemp(prefix="hf_storyboard_smoke_")
    ref = os.path.join(tmp, "ref.mp4")
    _make_ref_video(ref)

    # ---------- 1. storyboard CLI：5 个 Arc ----------
    sys.path.insert(0, TOOLS)
    import hf_storyboard
    import hf_compose
    import hf_validate
    for arc in hf_storyboard.ARCS:
        out = os.path.join(tmp, f"arc-{arc}.json")
        run([sys.executable, XMedia, "storyboard", arc,
             "--message", "三分钟看懂确定性渲染", "--audience", "开发者",
             "--mood", "沉稳", "-o", out])
        spec = json.load(open(out, encoding="utf-8"))
        n_frames = len(hf_storyboard.ARCS[arc]["frames"])
        assert len(spec.get("scenes") or []) == n_frames, \
            f"{arc}: 场景数 {len(spec.get('scenes'))} ≠ Arc 帧数 {n_frames}"
        sb = spec.get("storyboard") or {}
        assert sb.get("arc") == arc, f"{arc}: spec.storyboard.arc 缺失"
        sb_types = [f.get("type") for f in sb.get("frames", [])]
        assert all(t for t in sb_types), f"{arc}: 叙事词汇缺失"
        contract = os.path.join(tmp, f"arc-{arc}.html")
        run([sys.executable, os.path.join(TOOLS, "hf_compose.py"),
             out, "-o", contract])
        run([sys.executable, os.path.join(TOOLS, "hf_validate.py"),
             contract])
        # 叙事纪律（validate(html, spec)）：合规骨架零纪律警告；
        # 违规注入（背景/文字色偏离 + Math.random/repeat:-1）出 4 类警告
        import hf_validate
        html = open(contract, encoding="utf-8").read()
        f_ok, _, _, _ = hf_validate.validate(html, spec=spec)
        disc_ok = [m for lv, m in f_ok if lv == "warning" and
                   ("两色纪律" in m or "负面清单" in m or "breather" in m)]
        assert not disc_ok, f"{arc}: 合规骨架不应有纪律警告：{disc_ok}"
        bad = json.loads(json.dumps(spec))
        bad["background"] = "#ff0000"
        for sc in bad["scenes"]:
            sc["text_color"] = "#00ff00"
        bad_html = html.replace(
            "</body>",
            "<script>gsap.to(o,{repeat:-1});var r=Math.random();"
            "</script></body>")
        f_bad, _, _, _ = hf_validate.validate(bad_html, spec=bad)
        disc_bad = [m for lv, m in f_bad if lv == "warning" and
                    ("两色纪律" in m or "负面清单" in m or "breather" in m)]
        assert len(disc_bad) == 4, \
            f"{arc}: 违规注入应出 4 类纪律警告，实得 {disc_bad}"
    print(f"  ok storyboard CLI：{len(hf_storyboard.ARCS)} 个 Arc 全部"
          "骨架合法（compose/validate 0 errors + 叙事纪律双向断言）")

    # ---------- 2. Home 项目管理（--home） ----------
    projects_root = os.path.join(tmp, "projects")
    env = dict(os.environ, XMEDIA_STUDIO_PROJECTS=projects_root)
    home_log = open(os.path.join(tmp, "home-server.log"), "wb")
    server = subprocess.Popen(
        [sys.executable, HfStudio, "--serve", "--home", "--port", "8231"],
        env=env, stdout=home_log, stderr=subprocess.STDOUT)
    base = "http://127.0.0.1:8231"
    try:
        for _ in range(30):
            try:
                http("GET", base + "/api/projects", timeout=2)
                break
            except Exception:
                time.sleep(0.3)
        st, _, raw = http("GET", base + "/")
        assert st == 200 and "项目" in raw.decode("utf-8", "replace") and \
            "受众（可选）" in raw.decode("utf-8", "replace"), \
            "Home 页缺少项目管理标记"
        st, _, raw = http("GET", base + "/api/projects")
        assert jbody(raw)["projects"] == [], "初始项目列表应为空"
        st, _, raw = http("POST", base + "/api/project/create", body={
            "name": "smoke-home", "arc": "problem-solution",
            "message": "本地确定性渲染管线",
            "audience": "工程师"})
        create = jbody(raw)
        assert st == 200 and not create.get("error"), \
            f"create 失败：{create.get('error')}"
        pdir = os.path.join(projects_root, "smoke-home")
        assert os.path.isfile(os.path.join(pdir, "spec.json")), \
            "项目目录缺 spec.json"
        contract = os.path.join(pdir, "contract.html")
        assert os.path.isfile(contract), "项目目录缺 contract.html"
        run([sys.executable, os.path.join(TOOLS, "hf_validate.py"), contract])
        st, _, raw = http("GET", base + "/api/projects")
        names = [p["name"] if isinstance(p, dict) else p
                 for p in jbody(raw)["projects"]]
        assert "smoke-home" in names, f"项目列表缺 smoke-home：{names}"
        st, _, raw = http("GET", base + "/project/smoke-home")
        assert st == 200, "项目编辑页 404"
        print("  ok Home 项目管理：create → 骨架+契约+校验 → 列表/编辑页")
    finally:
        server.terminate()
        server.wait(timeout=10)
        home_log.close()

    # ---------- 3+4. reference 对话链 / 双屏对照（绑定契约页的服务） ----------
    contract = os.path.join(projects_root, "smoke-home", "contract.html")
    spec_file = os.path.join(projects_root, "smoke-home", "spec.json")
    ref_log = open(os.path.join(tmp, "ref-server.log"), "wb")
    server = subprocess.Popen(
        [sys.executable, HfStudio, contract, "--serve", "--spec",
         spec_file, "--port", "8232"],
        stdout=ref_log, stderr=subprocess.STDOUT)
    base = "http://127.0.0.1:8232"
    try:
        for _ in range(30):
            try:
                http("GET", base + "/api/projects", timeout=2)
                break
            except Exception:
                time.sleep(0.3)
        st, _, raw = http("POST", base + "/api/agent", body={
            "message": f"参考 {ref} 讲 测试主题"})
        agent_out = jbody(raw)
        ref_out = agent_out.get("reference") or {}
        assert not agent_out.get("error"), \
            f"reference 对话链失败：{agent_out.get('error')}"
        assert ref_out.get("ok") is not False, "reference action 未执行"
        st, _, raw = http("GET", base + "/api/state")
        state = jbody(raw)
        # ref.mp4 是三色硬切：场景检测应产出 3 镜（与 HF reference-video
        # 的"复刻剪辑节奏"对齐）
        assert len(state.get("scenes") or []) == 3, \
            f"同节奏骨架场景数 {len(state.get('scenes'))} ≠ 3"
        print("  ok reference 对话链：镜头切割 → 同节奏骨架写回（3 镜）")

        st, _, raw = http("POST", base + "/api/reference/video",
                          body={"path": ref})
        setref = jbody(raw)
        assert setref.get("url"), "reference/video 未返回 asset URL"
        st, _, raw = http("GET", base + "/api/state")
        assert jbody(raw).get("ref_video") == setref["url"], \
            "/api/state 的 ref_video 未同步"
        st, hdr, raw = http("GET", base + setref["url"],
                            headers={"Range": "bytes=0-99"})
        assert st == 206 and len(raw) == 100, \
            f"asset Range 响应异常：status={st}, len={len(raw)}"
        assert "Content-Range" in hdr, "206 缺 Content-Range"
        print("  ok 双屏对照：登记 → state.ref_video → asset Range/206")

        # ---------- 5. sb-frames 降级（对齐 /api/waveform 先例：
        # "环境渲染不出帧"返回 200 + 空帧 + note，不是错误——400 会让
        # 每次打开分镜都刷一屏 console 红字） ----------
        st, _, raw = http("GET", base + "/api/sb-frames")
        body = jbody(raw)
        assert st == 200 and body.get("ok") is True, \
            f"sb-frames 降级应为 200 ok：{st} {str(body)[:120]}"
        assert body.get("frames") == [], "降级响应应带空帧"
        assert "html_bake" in str(body.get("note", "")), \
            f"note 应指向 html_bake：{body.get('note')}"
        print("  ok sb-frames 降级：无 html_bake → 200 + 空帧 + 构建提示")
    finally:
        server.terminate()
        server.wait(timeout=10)
        ref_log.close()

    print("hf_storyboard_smoke: ALL PASS")


if __name__ == "__main__":
    main()
