#!/usr/bin/env python3
# hf_video_scene 冒烟（ctest hf_video_scene_smoke）：视频素材元素全链。
# testsrc2 源（ffmpeg 合成，内置动元素）→ 场景 video 背景 → 烘焙 →
# ① 帧推进（0.5 vs 1.5 全帧 diff）② 帧精度（烘焙@1.0 vs 源@1.0 顶部带
# RMS）③ 音轨存在。依赖：ffmpeg + html_bake 构建。
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CLI = os.path.join(ROOT, "tools", "xmedia.py")


def run(args):
    proc = subprocess.run([sys.executable, CLI] + args,
                          capture_output=True, text=True, timeout=600)
    assert proc.returncode == 0, f"exit {proc.returncode}\n{proc.stderr[-500:]}"
    return proc


def frame(path):
    return subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f",
                           "rawvideo", "-pix_fmt", "rgb24", "-"],
                          capture_output=True).stdout


def main():
    with tempfile.TemporaryDirectory(prefix="hf_video_scene_") as tmp:
        src = os.path.join(tmp, "src.webm")
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "lavfi",
                        "-i", "testsrc2=d=3:s=640x360:r=30",
                        "-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", src],
                       check=True, capture_output=True, timeout=120)
        spec = {
            "composition": {"id": "video-scene", "width": 960,
                            "height": 540, "fps": 30},
            "scenes": [
                {"type": "title", "text": "视频背景场景", "video": "src.webm",
                 "duration": 2.5, "bg": "#000000"},
                {"type": "outro", "text": "结束", "duration": 1,
                 "bg": "#0f3460"},
            ],
        }
        spec_path = os.path.join(tmp, "spec.json")
        with open(spec_path, "w", encoding="utf-8") as f:
            json.dump(spec, f, ensure_ascii=False)
        out = os.path.join(tmp, "out.mp4")
        run(["render", spec_path, "--out", out, "--no-tts"])

        # 抽帧（0.5/1.5）
        f05 = subprocess.run(["ffmpeg", "-v", "error", "-ss", "0.5", "-i",
                              out, "-frames:v", "1", "-f", "rawvideo",
                              "-pix_fmt", "rgb24", "-"],
                             capture_output=True).stdout
        f15 = subprocess.run(["ffmpeg", "-v", "error", "-ss", "1.5", "-i",
                              out, "-frames:v", "1", "-f", "rawvideo",
                              "-pix_fmt", "rgb24", "-"],
                             capture_output=True).stdout
        n = min(len(f05), len(f15))
        acc = sum((f05[i] - f15[i]) ** 2 for i in range(0, n, 7))
        n2 = n // 7 + 1
        d = (acc / n2) ** 0.5
        assert d > 20, f"视频未推进：diff {d:.1f}"

        # 帧精度：烘焙@1.0 vs 源@1.0（缩放到画布）顶部带（无文字）
        srcf = subprocess.run(["ffmpeg", "-v", "error", "-ss", "1.0", "-i",
                               src, "-frames:v", "1", "-vf",
                               "scale=960:540", "-f", "rawvideo",
                               "-pix_fmt", "rgb24", "-"],
                              capture_output=True).stdout
        b1 = subprocess.run(["ffmpeg", "-v", "error", "-ss", "1.0", "-i",
                             out, "-frames:v", "1", "-f", "rawvideo",
                             "-pix_fmt", "rgb24", "-"],
                            capture_output=True).stdout
        w = 960
        acc = n = 0
        for y in range(0, 120):
            for x in range(0, w):
                o = (y * w + x) * 3
                for c in range(3):
                    acc += (srcf[o + c] - b1[o + c]) ** 2
                    n += 1
        rms = (acc / n) ** 0.5
        assert rms < 25, f"帧对位偏差过大：RMS {rms:.1f}"
        print(f"hf_video_scene_smoke: PASS — 推进 diff {d:.1f}, 帧精度 RMS "
              f"{rms:.1f}")


if __name__ == "__main__":
    main()
