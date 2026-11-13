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
#   - 逐场景生成旁白 voice/scene-<i>.<ext>（幂等：已存在且可探测则跳过）
#   - 无 text 或已有 narration 的场景跳过（保留既有 narration）
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
# 环境变量兜底：TTS_PROVIDER / TTS_ENDPOINT（也读 OMNIVOICE_ENDPOINT，
# 对齐参考项目）。

import json
import os
import sys
import argparse
import subprocess
import urllib.request

# 复用 hf_compose 的 ffprobe 时长探测（不重复实现）。
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hf_compose import _probe_audio_duration


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
    with open(spec_path, "r", encoding="utf-8") as f:
        spec = json.load(f)

    tts = spec.get("tts", {})
    provider = (args.provider or os.environ.get("TTS_PROVIDER")
                or tts.get("provider")
                or ("say" if sys.platform == "darwin" else "http"))
    endpoint = (args.endpoint or os.environ.get("TTS_ENDPOINT")
                or os.environ.get("OMNIVOICE_ENDPOINT")
                or tts.get("endpoint") or "http://127.0.0.1:8123/tts")
    voice = args.voice if args.voice is not None else tts.get("voice")
    rate = args.rate if args.rate is not None else tts.get("rate")
    out_dir = args.out_dir or tts.get("out_dir") or "voice"

    abs_out_dir = out_dir if os.path.isabs(out_dir) else os.path.join(spec_dir, out_dir)
    os.makedirs(abs_out_dir, exist_ok=True)

    scenes = spec.get("scenes", [])
    ext = "wav" if provider == "say" else "mp3"
    generated = skipped = kept = 0
    for idx, scene in enumerate(scenes):
        text = (scene.get("text") or "").strip()
        if not text:
            continue
        if scene.get("narration"):
            kept += 1
            continue
        out_path = os.path.join(abs_out_dir, f"scene-{idx}.{ext}")
        rel = f"{out_dir}/{os.path.basename(out_path)}"
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
    else:
        stem = os.path.splitext(os.path.basename(spec_path))[0]
        out_spec = os.path.join(spec_dir, f"{stem}.narrated.json")
    with open(out_spec, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=2)

    print(f"hf_tts: {provider} — 生成 {generated}，幂等复用 {skipped}，"
          f"保留既有 {kept}；旁白目录 {out_dir} → {out_spec}")


if __name__ == "__main__":
    main()
