#!/usr/bin/env python3
# xmedia-kit ASR 字幕：把已录素材里的语音转成带时间戳的 caption segments。
#
# 设计原则（与管线其余部分一致）：零强制依赖、可插拔 provider、确定性输出。
#   - srt      : 直接读 SRT / WebVTT 字幕文件（纯标准库，永远可用）
#   - whisper  : whisper.cpp 的 whisper-cli（-oj 输出 JSON）
#   - fw       : faster-whisper Python 包（可选）
#   - auto     : 按 srt → whisper → fw 顺序尝试，都不可用则明确报错
#
# 输出统一为 {"source", "provider", "duration", "segments":[
#   {"start": 秒, "end": 秒, "text": "..."}]}，可直接放进 spec 顶层
#   `captions.segments`（全局时间轴绝对秒）或场景级 `captions`（场景相对秒）。
#
# 用法：
#   hf_asr.py clip.mp4 -o captions.json [--provider auto|srt|whisper|fw]
#   hf_asr.py clip.srt -o captions.json          # 按扩展名自动选 srt
#   hf_asr.py clip.mp4 --provider srt --srt clip.srt
#
# whisper.cpp 可执行文件用环境变量 XMEDIA_WHISPER_BIN 指定；模型用
# XMEDIA_WHISPER_MODEL 指定（whisper.cpp 需要）。faster-whisper 的模型尺寸
# 用 --model（默认 base）。所有子进程调用都走参数列表，不经过 shell。

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

MAX_SEGMENTS = 800
AUDIO_EXTS = (".wav", ".mp3", ".m4a", ".aac", ".flac", ".ogg")
SUBTITLE_EXTS = (".srt", ".vtt", ".webvtt")
VIDEO_EXTS = (".mp4", ".mov", ".mkv", ".avi", ".webm", ".m4v")


class AsrUnavailable(RuntimeError):
    """所有 ASR provider 都不可用（缺可执行文件 / 缺可选依赖 / 缺字幕文件）。"""


def parse_timestamp(value):
    """SRT「00:00:01,234」/ VTT「00:01.234」/ 裸秒「1.234」→ float 秒。"""
    s = str(value).strip().replace(",", ".")
    if re.fullmatch(r"\d+(\.\d+)?", s):
        return float(s)
    parts = s.split(":")
    if len(parts) == 3:
        h, m, sec = parts
        return int(h) * 3600 + int(m) * 60 + float(sec)
    if len(parts) == 2:
        m, sec = parts
        return int(m) * 60 + float(sec)
    raise ValueError(f"无法解析时间码：{value}")


def parse_srt(text):
    """解析 SRT / WebVTT → [{start, end, text}]。空行分块，时间戳行含 -->。"""
    blocks = re.split(r"\n\s*\n", text.replace("\r\n", "\n").replace("\r", "\n"))
    out = []
    for block in blocks:
        lines = [ln.strip() for ln in block.split("\n") if ln.strip()]
        if not lines:
            continue
        hit = next((i for i, ln in enumerate(lines) if "-->" in ln), None)
        if hit is None:
            continue
        m = re.search(r"([^\s>]+)\s*-->\s*([^\s>]+)", lines[hit])
        if not m:
            continue
        try:
            start = parse_timestamp(m.group(1))
            end = parse_timestamp(m.group(2))
        except ValueError:
            continue
        # 时间戳行之前可能有序号 / cue id，之后全是字幕文本
        body = " ".join(lines[hit + 1:])
        body = re.sub(r"<[^>]+>", "", body).strip()
        if body and end > start:
            out.append({"start": round(start, 3), "end": round(end, 3),
                        "text": body})
    return normalize_segments(out)


def parse_whisper_json(obj):
    """兼容 whisper.cpp -oj、OpenAI Whisper API、纯数组三种 JSON 形状。"""
    raw = None
    if isinstance(obj, list):
        raw = obj
    elif isinstance(obj, dict):
        if isinstance(obj.get("transcription"), list):
            # whisper.cpp: {"transcription":[{"timestamps":{...},"text":...}]}
            raw = obj["transcription"]
        elif isinstance(obj.get("segments"), list):
            raw = obj["segments"]
    if raw is None:
        raise ValueError("无法识别的 ASR JSON 结构")
    out = []
    for seg in raw:
        if not isinstance(seg, dict):
            continue
        text = str(seg.get("text") or "").strip()
        if not text:
            continue
        start = end = None
        off = seg.get("offsets")
        if isinstance(off, dict) and "from" in off and "to" in off:
            start, end = float(off["from"]) / 1000.0, float(off["to"]) / 1000.0
        ts = seg.get("timestamps")
        if (start is None or end is None) and isinstance(ts, dict):
            try:
                start = parse_timestamp(ts.get("from", "0"))
                end = parse_timestamp(ts.get("to", "0"))
            except (ValueError, TypeError):
                start = end = None
        if start is None or end is None:
            try:
                start, end = float(seg.get("start", 0)), float(seg.get("end", 0))
            except (TypeError, ValueError):
                continue
        if end > start:
            out.append({"start": round(start, 3), "end": round(end, 3),
                        "text": text})
    return normalize_segments(out)


def normalize_segments(segments, duration=None):
    """排序、去重、裁剪到时长、限制数量——保证同输入同输出。"""
    cleaned = []
    for seg in segments or []:
        if not isinstance(seg, dict):
            continue
        try:
            start = max(0.0, float(seg.get("start", 0)))
            end = float(seg.get("end", 0))
        except (TypeError, ValueError):
            continue
        text = re.sub(r"\s+", " ", str(seg.get("text") or "")).strip()
        if not text or end <= start:
            continue
        if duration is not None:
            start, end = min(start, duration), min(end, duration)
            if end <= start:
                continue
        cleaned.append({"start": round(start, 3), "end": round(end, 3),
                        "text": text[:300]})
    cleaned.sort(key=lambda s: (s["start"], s["end"], s["text"]))
    # 完全重复段（同起点同终点同文本）去掉，避免外部工具重复吐出
    uniq = []
    for seg in cleaned:
        if not uniq or seg != uniq[-1]:
            uniq.append(seg)
    return uniq[:MAX_SEGMENTS]


def probe_duration(path):
    """ffprobe 读时长；不可用时返回 None（不阻断字幕导入）。"""
    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "error", "-show_entries", "format=duration",
             "-of", "default=noprint_wrappers=1:nokey=1", path],
            capture_output=True, text=True, timeout=30)
        return round(float(proc.stdout.strip()), 3) if proc.returncode == 0 else None
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return None


def extract_audio(media, tmpdir):
    """任意媒体 → 16kHz 单声道 WAV（whisper.cpp 输入约定）。"""
    wav = os.path.join(tmpdir, "asr_audio.wav")
    proc = subprocess.run(
        ["ffmpeg", "-v", "error", "-y", "-i", media, "-vn",
         "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", wav],
        capture_output=True, text=True, timeout=300)
    if proc.returncode != 0 or not os.path.isfile(wav):
        raise AsrUnavailable(
            "ffmpeg 抽取音轨失败：" + (proc.stderr[-300:] or "无输出"))
    return wav


def whisper_binary(explicit=None):
    for candidate in (explicit, os.environ.get("XMEDIA_WHISPER_BIN"),
                      shutil.which("whisper-cli"), shutil.which("whisper"),
                      shutil.which("main")):
        if candidate and os.path.isfile(candidate):
            return candidate
    return None


def transcribe_srt(path):
    with open(path, "r", encoding="utf-8-sig", errors="replace") as f:
        segs = parse_srt(f.read())
    if not segs:
        raise AsrUnavailable(f"字幕文件里没有可解析的时间戳段：{path}")
    return segs


def transcribe_whisper(wav, language=None, binary=None, model=None,
                       timeout=600):
    exe = whisper_binary(binary)
    if not exe:
        raise AsrUnavailable(
            "找不到 whisper.cpp 可执行文件（设 XMEDIA_WHISPER_BIN 或安装 whisper-cli）")
    model = model or os.environ.get("XMEDIA_WHISPER_MODEL")
    if not model:
        raise AsrUnavailable(
            "whisper.cpp 需要模型文件（设 XMEDIA_WHISPER_MODEL=/path/ggml-*.bin）")
    cmd = [exe, "-m", model, "-f", wav, "-oj", "-of",
           os.path.join(os.path.dirname(wav), "asr_out")]
    if language:
        cmd += ["-l", language]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    out_json = os.path.join(os.path.dirname(wav), "asr_out.json")
    if proc.returncode != 0 or not os.path.isfile(out_json):
        raise AsrUnavailable(
            "whisper.cpp 执行失败：" + (proc.stderr[-300:] or proc.stdout[-300:]))
    with open(out_json, "r", encoding="utf-8") as f:
        return parse_whisper_json(json.load(f))


def _fw_cache_dir():
    """faster-whisper 模型缓存目录（默认 ~/.cache/xmedia-asr）。"""
    return (os.environ.get("XMEDIA_ASR_MODEL_DIR")
            or os.path.join(os.path.expanduser("~"), ".cache", "xmedia-asr"))


def default_model():
    """默认模型：优先本地已下载目录（免联网），否则用 HF 上的 small。

    中文口播用 small 明显好于 base，CPU int8 仍够快。可用 XMEDIA_ASR_MODEL
    覆盖（tiny/base/small/medium/large-v3 或本地模型目录路径）。
    """
    env = os.environ.get("XMEDIA_ASR_MODEL")
    if env:
        return env
    local = os.path.join(_fw_cache_dir(), "models", "small")
    if os.path.isfile(os.path.join(local, "model.bin")):
        return local
    return "small"


def transcribe_faster_whisper(wav, language=None, model=None, timeout=600):
    """faster-whisper（CTranslate2）本地转写。模型首次运行会下载到
    `XMEDIA_ASR_MODEL_DIR`（默认 ~/.cache/xmedia-asr）。

    国内网络常连不上 huggingface.co：未显式设置 HF_ENDPOINT 时，首次失败会
    自动改用 hf-mirror.com 重试一次，而不是直接报错。
    """
    try:
        from faster_whisper import WhisperModel
    except ImportError:
        raise AsrUnavailable(
            "faster-whisper 未安装（pip install faster-whisper），或改用 "
            "--provider srt 挂已有字幕")
    cache = _fw_cache_dir()
    try:
        os.makedirs(cache, exist_ok=True)
    except OSError:
        cache = None
    kwargs = {"device": "cpu", "compute_type": "int8"}
    try:
        # CPU 上默认只用 4 线程；吃满核心能快约 1.5~2 倍
        kwargs["cpu_threads"] = max(1, (os.cpu_count() or 4) - 1)
    except Exception:
        pass
    if cache:
        kwargs["download_root"] = cache
    try:
        engine = WhisperModel(model or default_model(), **kwargs)
    except Exception as e:  # 模型下载失败（网络 / 镜像）
        first = str(e)
        if os.environ.get("HF_ENDPOINT"):
            raise AsrUnavailable(
                f"faster-whisper 模型加载失败（{model or default_model()}）：{first[:200]}")
        os.environ["HF_ENDPOINT"] = "https://hf-mirror.com"
        try:
            engine = WhisperModel(model or default_model(), **kwargs)
        except Exception as e2:
            raise AsrUnavailable(
                f"faster-whisper 模型加载失败（{model or default_model()}）：{first[:120]}；"
                f"镜像重试仍失败：{str(e2)[:120]}")
    try:
        segs, _info = engine.transcribe(wav, language=language, vad_filter=True)
        return normalize_segments(
            [{"start": s.start, "end": s.end, "text": s.text} for s in segs])
    except Exception as e:
        raise AsrUnavailable(f"faster-whisper 转写失败：{str(e)[:200]}")


def transcribe(media, provider="auto", srt_path=None, language=None,
               model=None, binary=None, timeout=600):
    """统一入口 → {"source", "provider", "duration", "segments"}。"""
    media = os.path.abspath(media)
    if not os.path.isfile(media):
        raise AsrUnavailable(f"素材不存在：{media}")
    ext = os.path.splitext(media)[1].lower()
    duration = probe_duration(media) if ext not in SUBTITLE_EXTS else None

    srt = srt_path or (media if ext in SUBTITLE_EXTS else None)
    if srt and not os.path.isfile(srt):
        raise AsrUnavailable(f"字幕文件不存在：{srt}")

    chosen = provider
    if chosen == "auto":
        chosen = "srt" if srt else "whisper"
    if chosen == "srt":
        if not srt:
            raise AsrUnavailable("srt provider 需要 --srt 或 .srt/.vtt 输入")
        segs = transcribe_srt(srt)
        used = "srt"
    else:
        with tempfile.TemporaryDirectory(prefix="xmedia_asr_") as tmp:
            wav = (media if ext in AUDIO_EXTS else extract_audio(media, tmp))
            if wav.lower().endswith((".mp3", ".m4a", ".aac", ".flac", ".ogg")):
                wav = extract_audio(media, tmp)
            if chosen == "whisper":
                # auto：whisper.cpp → faster-whisper，两个都失败时把各自原因
                # 一起报出来（否则用户只看到最后一条，误以为只有一个后端）
                try:
                    segs = transcribe_whisper(wav, language, binary, model,
                                              timeout)
                    used = "whisper.cpp"
                except AsrUnavailable as e:
                    if provider != "auto":
                        raise
                    try:
                        segs = transcribe_faster_whisper(
                            wav, language, model, timeout)
                        used = "faster-whisper"
                    except AsrUnavailable as e2:
                        raise AsrUnavailable(
                            "没有可用的 ASR 后端：\n· whisper.cpp：" + str(e)
                            + "\n· faster-whisper：" + str(e2))
            elif chosen == "fw":
                segs = transcribe_faster_whisper(wav, language, model, timeout)
                used = "faster-whisper"
            else:
                raise AsrUnavailable(f"未知 ASR provider：{chosen}")
    segs = normalize_segments(segs, duration)
    if not segs:
        raise AsrUnavailable("ASR 未产出任何有效字幕段（可能是静音素材）")
    return {"source": media, "provider": used, "duration": duration,
            "segments": segs}


def transcribe_with_fallback(media, provider="auto", srt_path=None,
                             language=None, model=None, binary=None,
                             timeout=600):
    """Agent / 导入链路用的入口：先按 provider 真 ASR，缺依赖时自动降级到
    同目录的同名 SRT/VTT（或目录里唯一一份字幕），让「导入素材并加字幕」
    在零依赖环境也能跑通。结果里带 fallback_from 说明降级原因。"""
    primary_err = ""
    try:
        return transcribe(media, provider, srt_path, language, model,
                          binary, timeout)
    except (AsrUnavailable, ValueError, subprocess.TimeoutExpired) as e:
        primary_err = str(e)
    sub = srt_path
    if not sub:
        stem = os.path.splitext(media)[0]
        for ext in SUBTITLE_EXTS:
            if os.path.isfile(stem + ext):
                sub = stem + ext
                break
        if not sub:
            parent = os.path.dirname(media) or "."
            try:
                cands = [os.path.join(parent, n)
                         for n in sorted(os.listdir(parent))
                         if n.lower().endswith(SUBTITLE_EXTS)]
            except OSError:
                cands = []
            if len(cands) == 1:
                sub = cands[0]
    if sub and os.path.isfile(sub):
        segs = transcribe_srt(sub)
        duration = probe_duration(media)
        return {"source": sub, "provider": "srt-fallback",
                "duration": duration,
                "segments": normalize_segments(segs, duration),
                "fallback_from": primary_err}
    raise AsrUnavailable(primary_err)


def segments_for_scene(segments, scene_start, scene_duration):
    """全局时间轴字幕 → 场景相对时间，裁掉落在场景窗口外的部分。"""
    out = []
    for seg in segments or []:
        try:
            a, b = float(seg["start"]), float(seg["end"])
        except (KeyError, TypeError, ValueError):
            continue
        a = max(a, scene_start) - scene_start
        b = min(b, scene_start + scene_duration) - scene_start
        text = str(seg.get("text") or "").strip()
        if b > a and text:
            out.append({"start": round(a, 3), "end": round(min(b, scene_duration), 3),
                        "text": text})
    return normalize_segments(out, scene_duration)


def main():
    p = argparse.ArgumentParser(description="xmedia-kit ASR 字幕提取")
    p.add_argument("media", help="音视频素材，或 .srt/.vtt 字幕文件")
    p.add_argument("-o", "--out", default="", help="输出 JSON（默认 stdout）")
    p.add_argument("--provider", default="auto",
                   choices=["auto", "srt", "whisper", "fw"])
    p.add_argument("--srt", default="", help="外部 SRT/VTT 字幕文件")
    p.add_argument("--language", default="", help="语言代码（zh/en/...）")
    p.add_argument("--model", default="", help="whisper 模型路径 / fw 尺寸")
    p.add_argument("--whisper-bin", default="", help="whisper.cpp 可执行文件")
    args = p.parse_args()
    try:
        result = transcribe(args.media, args.provider, args.srt or None,
                            args.language or None, args.model or None,
                            args.whisper_bin or None)
    except (AsrUnavailable, ValueError, subprocess.TimeoutExpired) as e:
        print(f"hf_asr: {e}", file=sys.stderr)
        sys.exit(2)
    payload = json.dumps(result, ensure_ascii=False, indent=2)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(payload + "\n")
        print(f"hf_asr: {len(result['segments'])} 段字幕 → {args.out}"
              f"（provider={result['provider']}）")
    else:
        print(payload)


if __name__ == "__main__":
    main()
