#!/usr/bin/env python3
# 管线第 2 步：TTS / scene（AI-auto-generate-video 对齐）。
#
# 参考项目同样不实现 TTS 本体——外包给本地服务（OmniVoice，POST /tts
# 返回 audio/mpeg），逐场景幂等产出 voice/scene-<id>.mp3。本工具保持同
# 一架构：provider 抽象 + 本地服务，另加 macOS `say` 离线零依赖兜底。
#
# 用法：hf_tts.py <spec.json> [-o narrated.json] [--provider say|http]
#                 [--voice NAME] [--rate N] [--endpoint URL] [--out-dir DIR]
#
# 行为：
#   - 逐场景生成旁白 voice/scene-<i>-<文本哈希>.<ext>（幂等：同名存在
#     且可探测则跳过；文本哈希使词变自动重生成、跨 spec 不串音）
#   - 可朗读文本 = text 优先，缺失时按类型聚合专有字段（stats 取
#     value/description，quote 取 quote/author，list 取 items，
#     comparison 取列标题）；已有 narration 的场景跳过（保留既有旁白）
#   - 填 scene.narration → 输出 narrated spec（交 hf_compose 拟合时长）
#
# spec 的 tts 段（CLI 参数覆盖 spec）：
# {
#   "tts": {
#     "provider": "say",                            # say | http
#     "voice": "Tingting",                          # say 语音名 / http 透传
#     "rate": 180,                                  # say 每分钟词数 / http 透传
#     "endpoint": "http://127.0.0.1:8123/tts",      # OmniVoice 式本地服务
#     "out_dir": "voice"
#   }
# }
# 环境变量兜底（优先级 CLI > spec > 环境 > 默认）：TTS_PROVIDER /
# TTS_ENDPOINT（也读 OMNIVOICE_ENDPOINT，对齐参考项目）。

import hashlib
import json
import os
import re
import sys
import argparse
import subprocess
import urllib.request

# 复用 hf_compose 的 ffprobe 时长探测（不重复实现）。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hf_compose import _probe_audio_duration, tts_text


def say_tts(text, out_path, voice, rate):
    """macOS 离线 TTS（零依赖）：say → WAVE 16bit 22.05kHz。"""
    cmd = ["say", "-o", out_path, "--data-format=LEI16@22050"]
    if voice:
        cmd += ["-v", voice]
    if rate:
        cmd += ["-r", str(rate)]
    cmd.append(text)
    subprocess.run(cmd, check=True, capture_output=True, timeout=120)


def http_tts(text, out_path, voice, rate, endpoint):
    """OmniVoice 式本地服务：POST JSON {text, voice, rate} → audio 字节。"""
    payload = json.dumps({"text": text, "voice": voice or "",
                          "rate": rate or 0}).encode("utf-8")
    req = urllib.request.Request(
        endpoint, data=payload,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    if not data:
        raise RuntimeError(f"TTS 服务返回空音频：{endpoint}")
    with open(out_path, "wb") as f:
        f.write(data)


def tts_one(provider, text, out_path, voice, rate, endpoint):
    if provider == "say":
        say_tts(text, out_path, voice, rate)
    else:
        http_tts(text, out_path, voice, rate, endpoint)
    if _probe_audio_duration(out_path) is None:
        raise RuntimeError(f"TTS 产物无法探测时长：{out_path}")


def main():
    parser = argparse.ArgumentParser(description="HyperFrames 旁白 TTS 生成器")
    parser.add_argument("spec", help="spec JSON 文件路径")
    parser.add_argument("-o", "--output", default=None,
                        help="输出 narrated spec（默认 <stem>.narrated.json）")
    parser.add_argument("--provider", default=None, choices=["say", "http"])
    parser.add_argument("--voice", default=None)
    parser.add_argument("--rate", type=int, default=None)
    parser.add_argument("--endpoint", default=None)
    parser.add_argument("--out-dir", default=None, help="旁白输出目录（默认 voice）")
    args = parser.parse_args()

    spec_path = os.path.abspath(args.spec)
    spec_dir = os.path.dirname(spec_path) or "."
    try:
        with open(spec_path, "r", encoding="utf-8") as f:
            spec = json.load(f)
    except FileNotFoundError:
        print(f"hf_tts: spec 不存在：{spec_path}\n"
              "  先写 spec JSON（schema 见 docs/HF_COMPOSE.md），"
              "例如：{\"composition\": {...}, \"scenes\": [{\"type\": "
              "\"title\", \"text\": \"...\"}, ...]}", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"hf_tts: spec 不是合法 JSON：{e}", file=sys.stderr)
        sys.exit(1)

    tts = spec.get("tts", {})
    # 优先级：CLI > spec > 环境变量 > 默认（spec 是文件契约，环境是
    # 机器级兜底——环境压过 spec 会 surprises：全局 TTS_* 让该 spec 的
    # provider 选择失效）。
    provider = (args.provider or tts.get("provider")
                or os.environ.get("TTS_PROVIDER")
                or ("say" if sys.platform == "darwin" else "http"))
    endpoint = (args.endpoint or tts.get("endpoint")
                or os.environ.get("TTS_ENDPOINT")
                or os.environ.get("OMNIVOICE_ENDPOINT")
                or "http://127.0.0.1:8123/tts")
    voice = args.voice if args.voice is not None else tts.get("voice")
    rate = args.rate if args.rate is not None else tts.get("rate")
    out_dir = args.out_dir or tts.get("out_dir") or "voice"

    abs_out_dir = out_dir if os.path.isabs(out_dir) else os.path.join(spec_dir, out_dir)
    os.makedirs(abs_out_dir, exist_ok=True)

    scenes = spec.get("scenes", [])
    ext = "wav" if provider == "say" else "mp3"
    generated = skipped = kept = 0
    for idx, scene in enumerate(scenes):
        text = tts_text(scene)
        if not text:
            continue
        # 文件名带文本短哈希：词变了自动重生成；跨 spec 的同目录不串音
        # （同一 voice/ 下两个 spec 的 scene-N 不再按序号误复用）。
        text_hash = hashlib.sha1(text.encode("utf-8")).hexdigest()[:8]
        out_path = os.path.join(abs_out_dir,
                                f"scene-{idx}-{text_hash}.{ext}")
        rel = f"{out_dir}/{os.path.basename(out_path)}"
        narration = scene.get("narration")
        # 保留判定：用户自备旁白（非本工具命名 scene-N-<hash>）一律保留；
        # 本工具命名但哈希不匹配 = 词已变 → 重生成。
        stale = False
        if narration:
            m2 = re.match(rf"scene-{idx}-([0-9a-f]{{8}})\.",
                          os.path.basename(narration))
            stale = bool(m2 and m2.group(1) != text_hash)
        if narration and not stale:
            kept += 1
            continue
        if os.path.isfile(out_path) and _probe_audio_duration(out_path) is not None:
            scene["narration"] = rel  # 幂等：已生成则直接挂路径
            skipped += 1
            continue
        try:
            tts_one(provider, text, out_path, voice, rate, endpoint)
        except (subprocess.CalledProcessError, RuntimeError, OSError) as e:
            print(f"hf_tts: scene-{idx} 失败：{e}", file=sys.stderr)
            sys.exit(1)
        scene["narration"] = rel
        generated += 1

    if args.output:
        out_spec = args.output
        stem = os.path.splitext(os.path.basename(out_spec))[0]
        stem = stem[: -len(".narrated")] if stem.endswith(".narrated") else stem
    else:
        stem = os.path.splitext(os.path.basename(spec_path))[0]
        out_spec = os.path.join(spec_dir, f"{stem}.narrated.json")
    with open(out_spec, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=2)

    # 交付产物（参考管线第 2/4 步对齐）：旁白全文 script.txt（供字幕工具）
    # + 按场景起点定位拼接的独立旁白轨 voice.m4a（场景起点 = 旁白实长 +
    # padding 累计，与 hf_compose 的拟合公式同源）。
    clips = []
    texts = []
    start_ms = 0
    for idx, scene in enumerate(spec.get("scenes", [])):
        narration = scene.get("narration")
        if not narration:
            continue
        path = (narration if os.path.isabs(narration)
                else os.path.join(spec_dir, narration))
        dur = _probe_audio_duration(path)
        if dur is None:
            continue
        clips.append((path, start_ms))
        texts.append(tts_text(scene))
        start_ms += int(round((dur + float(scene.get("padding", 0.5))) * 1000))
    if clips:
        script_txt = os.path.join(spec_dir, f"{stem}.script.txt")
        with open(script_txt, "w", encoding="utf-8") as f:
            f.write("\n".join(texts) + "\n")
        voice_track = os.path.join(spec_dir, f"{stem}.voice.m4a")
        cmd = ["ffmpeg", "-y", "-v", "error"]
        for path, _ in clips:
            cmd += ["-i", path]
        parts = [f"[{i}:a]adelay={ms}:all=1[a{i}]"
                 for i, (_, ms) in enumerate(clips)]
        fc = (";".join(parts) + ";" + "".join(f"[a{i}]" for i in range(len(clips)))
              + f"amix=inputs={len(clips)}:normalize=0:duration=longest[aout]")
        cmd += ["-filter_complex", fc, "-map", "[aout]",
                "-c:a", "aac", "-b:a", "192k", voice_track]
        subprocess.run(cmd, check=True, capture_output=True, timeout=300)
        print(f"hf_tts: 旁白轨 {voice_track}（{len(clips)} 段定位拼接）"
              f" + 字幕文本 {script_txt}")

    print(f"hf_tts: {provider} — 生成 {generated}，幂等复用 {skipped}，"
          f"保留既有 {kept}；旁白目录 {out_dir} → {out_spec}")


if __name__ == "__main__":
    main()
