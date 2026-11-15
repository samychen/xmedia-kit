#!/usr/bin/env python3
# xmedia-kit 素材自动导入：扫一个目录里的实拍视频 → 生成 spec JSON。
#
# 对位 Remotion「agentically」演示里那句：
#   "Please import the videos I just recorded from ~/Downloads and put them
#    on a timeline, caption it." → "add our endcard to it"
#
# 用 ffprobe 读每个素材的实长/尺寸，按文件名确定性排序逐段排到时间轴；
# --asr 顺带跑语音转写，把分段字幕按场景起点偏移到全局时间轴；
# --endcard 追加内置 outro-brand 品牌收尾卡模板。
#
# 生成的是普通 spec JSON，之后走和手写 spec 完全相同的
# TTS → compose → validate → Studio → bake 链路，来源锚点照常可验。
#
# 用法：
#   hf_import.py ~/Downloads -o my_video.json --asr --endcard
#   hf_import.py ~/Downloads -o my_video.json --captions clip.srt --format 9:16

import argparse
import json
import os
import subprocess
import sys

VIDEO_EXTS = (".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v")
AUDIO_EXTS = (".wav", ".mp3", ".m4a", ".aac", ".flac", ".ogg")
SKIP_NAMES = (".ds_store", "thumbs.db")


class ImportError_(RuntimeError):
    """导入失败（目录不存在 / 无可用素材 / ffprobe 不可用）。"""


def probe(path):
    """ffprobe → {duration, width, height, has_audio, has_video}。"""
    cmd = ["ffprobe", "-v", "error", "-show_entries",
           "format=duration:stream=codec_type,width,height",
           "-of", "json", path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise ImportError_(f"ffprobe 不可用：{e}")
    if proc.returncode != 0:
        raise ImportError_(f"ffprobe 读取失败：{path} "
                           f"{proc.stderr[-200:]}")
    try:
        meta = json.loads(proc.stdout)
    except json.JSONDecodeError:
        raise ImportError_(f"ffprobe 输出不是 JSON：{path}")
    streams = meta.get("streams") or []
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    audio = next((s for s in streams if s.get("codec_type") == "audio"), None)
    try:
        duration = round(float(meta.get("format", {}).get("duration") or 0), 3)
    except (TypeError, ValueError):
        duration = 0.0
    return {"duration": duration,
            "width": int((video or {}).get("width") or 0),
            "height": int((video or {}).get("height") or 0),
            "has_audio": audio is not None,
            "has_video": video is not None}


def scan(src_dir):
    """确定性扫描：文件名排序，返回 (视频列表, 音频列表)。

    src_dir 也可以是单个音视频文件——直接该文件成一段素材。
    """
    # 单个文件：按扩展名归入视频 / 音频，等价于「只含一个素材的目录」。
    if os.path.isfile(src_dir):
        ext = os.path.splitext(src_dir)[1].lower()
        if ext in VIDEO_EXTS:
            return [src_dir], []
        if ext in AUDIO_EXTS:
            return [], [src_dir]
        raise ImportError_(
            f"不支持该文件类型：{os.path.basename(src_dir)}（可导入 "
            f"{' / '.join(VIDEO_EXTS + AUDIO_EXTS)}）")
    if not os.path.isdir(src_dir):
        raise ImportError_(f"路径不存在：{src_dir}")
    videos, audios = [], []
    for name in sorted(os.listdir(src_dir)):
        if name.startswith(".") or name.lower() in SKIP_NAMES:
            continue
        path = os.path.join(src_dir, name)
        if not os.path.isfile(path):
            continue
        ext = os.path.splitext(name)[1].lower()
        if ext in VIDEO_EXTS:
            videos.append(path)
        elif ext in AUDIO_EXTS:
            audios.append(path)
    if not videos and not audios:
        raise ImportError_(f"目录里没有可导入的音视频素材：{src_dir}")
    return videos, audios


def rel_to(path, spec_dir):
    """相对 spec 目录的路径；跨目录（relpath 深 .. 链）时用绝对路径。"""
    try:
        rel = os.path.relpath(os.path.abspath(path), os.path.abspath(spec_dir))
    except ValueError:
        return os.path.abspath(path)
    return os.path.abspath(path) if rel.startswith("..") else rel


def load_caption_file(path):
    """读 hf_asr 产出的 captions.json / SRT / VTT → 全局字幕段。"""
    import hf_asr
    ext = os.path.splitext(path)[1].lower()
    if ext == ".json":
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        raw = data.get("segments", []) if isinstance(data, dict) else data
        return hf_asr.normalize_segments(raw), "captions.json"
    return hf_asr.transcribe_srt(path), "srt"


def build_spec(src_dir, spec_dir=None, fmt="16:9", fps=30, asr=False,
               captions_file="", endcard=False, brand="xmedia-kit",
               tagline="", bg="#000000", sfx=False, provider="auto"):
    """→ (spec dict, notes list)。notes 是给 Agent / CLI 的人类可读摘要。

    src_dir 可以是目录，也可以是单个音视频文件（一段场景）。
    """
    if not spec_dir:
        # 单文件导入时 spec 目录默认取该文件所在目录，相对路径才写得出来。
        spec_dir = (os.path.dirname(os.path.abspath(src_dir))
                    if os.path.isfile(src_dir) else src_dir)
    spec_dir = os.path.abspath(spec_dir)
    videos, audios = scan(src_dir)
    notes = []
    scenes = []
    global_caps = []
    cursor = 0.0

    for path in videos:
        info = probe(path)
        dur = info["duration"]
        if dur <= 0:
            notes.append(f"跳过（无法探测时长）：{os.path.basename(path)}")
            continue
        # 实拍素材保留默认入场动画（GSAP 时间线里 0.6s fade_in），
        # 不额外叠加合成特效；video 元素由烘焙侧原生视频层解码。
        scene = {"type": "text", "text": "", "duration": dur,
                 "video": rel_to(path, spec_dir), "bg": bg}
        scenes.append(scene)
        if asr:
            try:
                import hf_asr
                res = hf_asr.transcribe_with_fallback(path, provider=provider)
                for seg in res["segments"]:
                    global_caps.append({
                        "start": round(cursor + seg["start"], 3),
                        "end": round(cursor + seg["end"], 3),
                        "text": seg["text"]})
                notes.append(f"ASR {os.path.basename(path)}："
                             f"{len(res['segments'])} 段（{res['provider']}）")
                if res.get("fallback_from"):
                    notes.append("  ASR 不可用，已降级到同目录字幕："
                                 + res["fallback_from"][:120])
            except Exception as e:
                notes.append(f"ASR 跳过 {os.path.basename(path)}：{e}")
                notes.append("  仍导入了画面；要出字幕可 pip install "
                             "faster-whisper 后重试，或放一个同名 .srt 再导入")
        cursor += dur

    if not scenes:
        raise ImportError_("没有可用的视频素材（时长均不可探测）")

    if captions_file:
        if not os.path.isfile(captions_file):
            raise ImportError_(f"字幕文件不存在：{captions_file}")
        segs, origin = load_caption_file(captions_file)
        if not global_caps:
            global_caps = [dict(s) for s in segs]
        else:
            # 已有 ASR 段时，外部字幕作为追加（按时间归并后规范化）
            global_caps += [dict(s) for s in segs]
        notes.append(f"外部字幕 {origin}：{len(segs)} 段")

    if endcard:
        scenes.append({"type": "outro-brand", "brand": brand,
                       "tagline": tagline or "Made with xmedia-kit",
                       "duration": 2.5, "bg": "#0f3460"})
        notes.append("已追加品牌片尾卡 outro-brand")

    spec = {
        "schema_version": "1.0",
        "composition": {"id": "imported", "format": fmt, "fps": fps},
        "background": bg,
        # 导入实拍素材默认不做交叉转场，保证场景起点 = 时长累加，
        # 全局字幕段的时间戳才与画面严格对齐。
        "transitions": {"type": "none", "duration": 0},
        "sfx": {"enabled": bool(sfx)},
        "scenes": scenes,
    }
    if global_caps:
        import hf_asr
        spec["captions"] = {"enabled": True,
                            "segments": hf_asr.normalize_segments(
                                global_caps, cursor + 3.0)}
    if audios:
        spec["audio"] = {"src": rel_to(audios[0], spec_dir),
                         "volume": 0.35, "fade_in": 0.5, "fade_out": 1.0}
        notes.append(f"BGM：{os.path.basename(audios[0])}")
    notes.insert(0, f"导入 {len(scenes)} 个场景，总时长 {round(cursor, 3)}s")
    return spec, notes


def main():
    p = argparse.ArgumentParser(
        description="xmedia-kit 素材自动导入（目录 → spec JSON）")
    p.add_argument("dir", help="素材目录")
    p.add_argument("-o", "--out", default="", help="输出 spec JSON（默认 stdout）")
    p.add_argument("--format", default="16:9",
                   choices=["16:9", "9:16", "1:1", "4:5"])
    p.add_argument("--fps", type=int, default=30)
    p.add_argument("--asr", action="store_true", help="逐素材跑 ASR 自动字幕")
    p.add_argument("--captions", default="", help="外部 SRT/VTT/captions.json")
    p.add_argument("--endcard", action="store_true", help="追加品牌片尾卡")
    p.add_argument("--brand", default="xmedia-kit")
    p.add_argument("--tagline", default="")
    p.add_argument("--bg", default="#000000")
    p.add_argument("--sfx", action="store_true", help="启用内置 SFX（默认关）")
    p.add_argument("--provider", default="auto",
                   choices=["auto", "srt", "whisper", "fw"])
    args = p.parse_args()
    out = os.path.abspath(args.out) if args.out else ""
    spec_dir = os.path.dirname(out) if out else args.dir
    try:
        spec, notes = build_spec(args.dir, spec_dir, args.format, args.fps,
                                 args.asr, args.captions, args.endcard,
                                 args.brand, args.tagline, args.bg, args.sfx,
                                 args.provider)
    except (ImportError_, OSError, ValueError) as e:
        print(f"hf_import: {e}", file=sys.stderr)
        sys.exit(2)
    payload = json.dumps(spec, ensure_ascii=False, indent=2)
    if out:
        os.makedirs(spec_dir, exist_ok=True)
        with open(out, "w", encoding="utf-8") as f:
            f.write(payload + "\n")
        print(f"hf_import: spec → {out}")
    else:
        print(payload)
    for n in notes:
        print(f"hf_import: {n}", file=sys.stderr)


if __name__ == "__main__":
    main()
