#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""参考视频 → 同节奏 spec 骨架（HF Studio app reference-video 的本地弱化版）。

HF 的 reference-video 用云视频理解"读片复刻"（自动复制剪辑/字幕/特效，
依赖 HeyGen Pro credits）。本模块是**纯本地弱化版**，只做三件确定性的事：

  1. ffmpeg 场景检测把参考视频切成镜头（shots）——保留原片的剪辑节奏；
  2. 每镜抽一帧缩略图，生成逐镜对照表（shots.json）；
  3. 生成同节奏的 spec 骨架：镜头数=场景数、时长=镜头时长，文案留占位
     等你填；可选 --asr 用 hf_asr 把参考音频转成全局字幕段（对齐
     spec.captions.segments 的绝对秒语义）。

不复刻视觉特效、不换脸不换声——那些是云能力（HF_COMPOSE.md 对齐表
"明确不做"一节）。产出的 spec 直接可 `xmedia render`（占位文案），
改完文案/换成自己的素材，就是你的视频。

与 hf_import 的关系：hf_import 把素材"搬进"工程；本模块把参考视频
"读成"工程骨架。

单位约定（与仓库一致）：一切时间都是**秒**。
"""

import argparse
import json
import os
import re
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))


def _run(cmd, timeout=120):
    # 托管运行时会注入 LD_LIBRARY_PATH（runtime 自带库），与系统 ffmpeg 的
    # 插件依赖（rubberband/zimg 等 GLIBCXX 版本）冲突——清空后系统 ffmpeg
    # 正常（实测 lavfi 合成 + 场景检测均过）。ffprobe 同样受益。
    env = os.environ.copy()
    env.pop("LD_LIBRARY_PATH", None)
    return subprocess.run(cmd, capture_output=True, text=True,
                          timeout=timeout, env=env)


def _ffprobe_meta(video):
    """ffprobe → {duration, width, height}。"""
    r = _run(["ffprobe", "-v", "error", "-print_format", "json",
              "-show_format", "-show_streams", video])
    if r.returncode != 0:
        raise RuntimeError(f"ffprobe 失败：{r.stderr.strip()[:200]}")
    info = json.loads(r.stdout or "{}")
    duration = float(info.get("format", {}).get("duration", 0) or 0)
    width = height = 0
    for st in info.get("streams", []):
        if st.get("codec_type") == "video":
            width = int(st.get("width") or 0)
            height = int(st.get("height") or 0)
            break
    if duration <= 0:
        raise RuntimeError("ffprobe 读不到时长（文件损坏或不是媒体文件）")
    return {"duration": duration, "width": width, "height": height}


def detect_shots(video, threshold=0.3, min_shot=0.6, duration=None):
    """ffmpeg 场景检测 → [(start, dur), ...]（秒）。

    select=gt(scene,T) 输出镜头切换点；showinfo 的 pts_time 是切换时刻。
    短于 min_shot 的镜头并入前一镜（保留原片节奏，不产生碎镜）。
    """
    r = _run(["ffmpeg", "-i", video,
              "-vf", f"select='gt(scene,{threshold})',showinfo",
              "-an", "-f", "null", "-"], timeout=600)
    # showinfo 行在 stderr；pts_time 是切换点时刻
    cuts = [float(m) for m in
            re.findall(r"pts_time:([0-9.]+)", r.stderr or "")]
    if duration is None:
        duration = _ffprobe_meta(video)["duration"]
    cuts = sorted(t for t in cuts if 0 < t < duration)
    bounds = [0.0] + cuts + [duration]
    shots = []
    for a, b in zip(bounds, bounds[1:]):
        d = round(b - a, 3)
        if d <= 0:
            continue
        if shots and d < min_shot:
            # 碎镜并入前一镜：只延长前镜，不新增场景
            s, pd = shots[-1]
            shots[-1] = (s, round(b - s, 3))
        else:
            shots.append((round(a, 3), d))
    return shots


def extract_thumbs(video, shots, outdir):
    """每镜抽一帧（取镜头 40% 处，避开切帧噪声）→ thumb_NN.jpg。"""
    os.makedirs(outdir, exist_ok=True)
    thumbs = []
    for i, (start, dur) in enumerate(shots):
        at = max(0.0, start + dur * 0.4)
        out = os.path.join(outdir, f"thumb_{i:02d}.jpg")
        r = _run(["ffmpeg", "-y", "-ss", f"{at:.3f}", "-i", video,
                  "-frames:v", "1", "-vf", "scale=320:-2", out], timeout=60)
        if r.returncode == 0 and os.path.isfile(out):
            thumbs.append(os.path.basename(out))
    return thumbs


def maybe_asr(video, outdir):
    """可选：hf_asr 把参考音频转全局字幕段。provider 缺失 → 返回 None。"""
    caps_path = os.path.join(outdir, "captions.json")
    r = _run([sys.executable, os.path.join(TOOLS_DIR, "hf_asr.py"),
              video, "-o", caps_path], timeout=600)
    if r.returncode != 0 or not os.path.isfile(caps_path):
        return None, (r.stderr or "").strip()[-300:]
    data = json.load(open(caps_path, encoding="utf-8"))
    segs = []
    for s in data.get("segments", []):
        text = str(s.get("text") or "").strip()
        start = float(s.get("start") or 0)
        dur = float(s.get("dur") or s.get("end", 0) - start or 0)
        if text and dur > 0:
            segs.append({"text": text, "start": round(start, 3),
                         "dur": round(max(dur, 0.3), 3)})
    return (segs or None), None


def _fmt_tc(sec):
    m, s = divmod(max(0, sec), 60)
    return f"{int(m):d}:{s:04.1f}"


def build_spec(shots, video, meta, captions=None, name=None, voice="Tingting"):
    """同节奏 spec 骨架：镜头数=场景数、时长=镜头时长。"""
    stem = os.path.splitext(os.path.basename(video))[0]
    w, h = meta.get("width", 1080), meta.get("height", 1920)
    if w and h:
        ratio = w / h
        fmt = ("9:16" if ratio < 0.7 else
               "1:1" if ratio < 0.95 else
               "4:5" if ratio < 1.1 else "16:9")
    else:
        fmt = "16:9"
    palette = ["#1a1a2e", "#16213e", "#0f3460"]
    scenes = []
    for i, (start, dur) in enumerate(shots):
        scenes.append({
            "type": "text",
            "text": f"镜头 {i + 1}（参考 {_fmt_tc(start)}–{_fmt_tc(start + dur)}）",
            "subtitle": "在这里写这一镜的文案",
            "duration": round(dur, 2),
            "bg": palette[i % len(palette)],
            "animation": "fade_in",
        })
    spec = {
        "schema_version": "1.0",
        "composition": {"id": name or f"reference-{stem[:24]}",
                        "format": fmt, "fps": 30},
        "background": "#0a0a1a",
        "scenes": scenes,
    }
    if captions:
        spec["captions"] = {"enabled": True, "segments": captions}
    return spec


def run(video, out=None, max_scenes=12, threshold=0.3, min_shot=0.6,
        do_asr=False, name=None):
    """主入口 → (spec_path, 报告 dict)。"""
    if not os.path.isfile(video):
        raise RuntimeError(f"参考视频不存在：{video}")
    meta = _ffprobe_meta(video)
    shots = detect_shots(video, threshold=threshold, min_shot=min_shot,
                         duration=meta["duration"])
    if not shots:
        raise RuntimeError("场景检测没找到任何镜头（提高 --threshold 试试）")
    # 超过 max_scenes：把最短的镜头逐个并入右邻，保总时长与节奏
    while len(shots) > max_scenes:
        idx = min(range(len(shots)), key=lambda i: shots[i][1])
        if idx == len(shots) - 1:
            idx -= 1
        s, d = shots[idx]
        s2, d2 = shots[idx + 1]
        shots[idx:idx + 2] = [(s, round(s2 + d2 - s, 3))]

    stem = os.path.splitext(os.path.basename(video))[0]
    out = out or os.path.join("reference", stem)
    os.makedirs(out, exist_ok=True)
    thumbs_dir = os.path.join(out, "thumbs")
    thumbs = extract_thumbs(video, shots, thumbs_dir)

    captions, asr_note = (None, None)
    if do_asr:
        captions, asr_note = maybe_asr(video, out)

    spec = build_spec(shots, video, meta, captions=captions, name=name)
    spec_path = os.path.join(out, "spec.json")
    with open(spec_path, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
        f.write("\n")

    shots_doc = {
        "source": os.path.abspath(video),
        "duration": meta["duration"],
        "resolution": [meta["width"], meta["height"]],
        "threshold": threshold,
        "shots": [{"index": i, "start": s, "dur": d,
                   "thumb": ("thumbs/" + thumbs[i]) if i < len(thumbs) else None}
                  for i, (s, d) in enumerate(shots)],
    }
    shots_path = os.path.join(out, "shots.json")
    with open(shots_path, "w", encoding="utf-8") as f:
        json.dump(shots_doc, f, ensure_ascii=False, indent=1)
        f.write("\n")

    report = {
        "video": video, "duration": round(meta["duration"], 2),
        "shots": len(shots), "thumbs": len(thumbs),
        "spec": spec_path, "shots_json": shots_path,
        "captions": bool(captions), "asr_note": asr_note,
    }
    return spec_path, report


def main():
    p = argparse.ArgumentParser(
        description="参考视频 → 同节奏 spec 骨架（reference-video 本地弱化版）")
    p.add_argument("video", help="参考视频文件（本地文件；链接下载不在本工具范围）")
    p.add_argument("-o", "--out", default="",
                   help="输出目录（默认 reference/<视频名>/）")
    p.add_argument("--max-scenes", type=int, default=12,
                   help="场景数上限（超出时最短镜头并入右邻，默认 12）")
    p.add_argument("--threshold", type=float, default=0.3,
                   help="场景切换检测阈值 0-1（越高切得越少，默认 0.3）")
    p.add_argument("--min-shot", type=float, default=0.6,
                   help="最短镜头秒数（更短并入前镜，默认 0.6）")
    p.add_argument("--asr", action="store_true",
                   help="用 hf_asr 把参考音频转成全局字幕段（需本机 provider）")
    p.add_argument("--name", default="", help="合成 id（默认 reference-<视频名>）")
    args = p.parse_args()
    try:
        spec_path, report = run(
            args.video, out=args.out or None, max_scenes=args.max_scenes,
            threshold=args.threshold, min_shot=args.min_shot,
            do_asr=args.asr, name=args.name or None)
    except (RuntimeError, subprocess.TimeoutExpired) as e:
        print(f"hf_reference: {e}", file=sys.stderr)
        sys.exit(2)
    print(f"hf_reference: {report['shots']} 镜 / {report['duration']}s"
          f" / 缩略图 {report['thumbs']} 张"
          f"{' / 字幕 ' + str(len(captions)) + ' 段' if report['captions'] else ''}")
    if report["asr_note"]:
        print(f"hf_reference: ASR 跳过——{report['asr_note']}", file=sys.stderr)
    print(f"  spec     {report['spec']}")
    print(f"  对照表   {report['shots_json']}")
    print("  下一步：改 spec 里的占位文案/换成自己的素材 → xmedia render")


if __name__ == "__main__":
    main()
