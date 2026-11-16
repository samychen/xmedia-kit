#!/usr/bin/env python3
# hf_split_scene 冒烟（ctest hf_split_scene_smoke）：时间轴片段切割的
# 渲染侧闭环。同一条素材切成前后两段（后段 video_from=1.5）：
#   ① compose 只在 video_from>0 的 <video> 上写 data-media-start，且单位与
#      data-start 一致（秒）——写成毫秒会被采集端再乘 1000；
#   ② 烘焙后「后段 @local 0.5」必须与「源 @2.0」对位（video_from + local）；
#   ③ 前段 @0.5 仍与源 @0.5 对位——偏移不能串到前一段。
# 依赖：ffmpeg + html_bake 构建。
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import hf_compose  # noqa: E402
import xmedia  # noqa: E402


def frame_at(path, sec, w=640, h=360):
    return subprocess.run(["ffmpeg", "-v", "error", "-ss", str(sec), "-i", path,
                           "-frames:v", "1", "-vf", f"scale={w}:{h}",
                           "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                          capture_output=True).stdout


def top_rms(a, b, w=640, rows=90, step=2):
    """顶部带 RMS：文字压在画面中心，顶部只有视频内容，比对最干净。"""
    acc = n = 0
    for y in range(0, rows, step):
        for x in range(0, w, step):
            o = (y * w + x) * 3
            for c in range(3):
                acc += (a[o + c] - b[o + c]) ** 2
                n += 1
    return (acc / n) ** 0.5


def main():
    bake = xmedia.bake_bin()
    if not bake:
        # 干净 clone 上 ctest 会注册本用例但 html_bake 还没构建（或 CEF 关）。
        # 跳过要说得明白：这是"没构建"，不是"通过了"。
        print("hf_split_scene_smoke: SKIP — html_bake 未构建"
              "（cmake --build build --target html_bake）")
        return 0
    with tempfile.TemporaryDirectory(prefix="hf_split_scene_") as tmp:
        src = os.path.join(tmp, "src.mp4")
        subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "lavfi",
                        "-i", "testsrc2=d=3:s=640x360:r=30",
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", src],
                       check=True, capture_output=True, timeout=120)
        spec = {
            "schema_version": "1.0",
            "composition": {"id": "split", "format": "16:9", "fps": 30},
            "transitions": {"type": "none", "duration": 0},
            "sfx": {"enabled": False},
            "scenes": [
                {"type": "text", "text": "", "duration": 1.0, "video": src},
                {"type": "text", "text": "", "duration": 1.0, "video": src,
                 "video_from": 1.5},
            ],
        }
        html = hf_compose.compose(spec, spec_name="split.json")
        vids = re.findall(r"<video[^>]*>", html)
        assert len(vids) == 2, vids
        assert "data-media-start" not in vids[0], \
            f"video_from 缺省不应写 data-media-start：{vids[0]}"
        assert 'data-media-start="1.5"' in vids[1], \
            f"data-media-start 必须是秒且等于 video_from：{vids[1]}"

        html_path = os.path.join(tmp, "split.html")
        with open(html_path, "w", encoding="utf-8") as f:
            f.write(html)
        out = os.path.join(tmp, "out.mp4")
        subprocess.run([bake, "--url", html_path, "--out", out,
                        "--seconds", "2", "--fps", "30",
                        "--width", "640", "--height", "360",
                        "--clock", "seek"],
                       check=True, capture_output=True, timeout=600)

        src05 = frame_at(src, 0.5)
        src20 = frame_at(src, 2.0)
        front = frame_at(out, 0.5)   # 前段 local 0.5 → 源 0.5
        back = frame_at(out, 1.5)    # 后段 local 0.5 → 源 2.0（1.5 + 0.5）
        assert front and back, "抽帧失败"
        rms_front = top_rms(front, src05)
        rms_back = top_rms(back, src20)
        rms_wrong = top_rms(back, src05)  # 位移没生效时后段会落到这里
        assert rms_front < 25, f"前段帧对位偏差：RMS {rms_front:.1f}"
        assert rms_back < 25, (
            f"后段未按 video_from 取帧：对源@2.0 RMS {rms_back:.1f} "
            f"（对照：对源@0.5 RMS {rms_wrong:.1f}）")
        assert rms_back < rms_wrong, (
            f"后段更像源@0.5 而不是源@2.0：{rms_back:.1f} vs {rms_wrong:.1f}")

        # ② 接缝转场：给后段显式 fade，交叉窗内必须是两段素材的混色。
        #    原生视频层只按矩形出帧、不理会场景容器 opacity 的话，交叉
        #    淡化在成片里就是硬切（"转场没反应"）——用蓝→红锁住这条链路。
        blue = os.path.join(tmp, "blue.mp4")
        red = os.path.join(tmp, "red.mp4")
        for color, path in (("blue", blue), ("red", red)):
            subprocess.run(["ffmpeg", "-y", "-v", "error", "-f", "lavfi",
                            "-i", f"color=c={color}:s=640x360:r=30:d=3",
                            "-c:v", "libx264", "-pix_fmt", "yuv420p", path],
                           check=True, capture_output=True, timeout=120)
        trans_spec = {
            "schema_version": "1.0",
            "composition": {"id": "xfade", "format": "16:9", "fps": 30},
            "background": "#000000",
            "transitions": {"type": "fade", "duration": 0.6},
            "sfx": {"enabled": False},
            "scenes": [
                {"type": "text", "text": "", "duration": 2.0, "video": blue},
                {"type": "text", "text": "", "duration": 2.0, "video": red},
            ],
        }
        trans_html = os.path.join(tmp, "xfade.html")
        with open(trans_html, "w", encoding="utf-8") as f:
            f.write(hf_compose.compose(trans_spec, spec_name="xfade.json"))
        trans_out = os.path.join(tmp, "xfade.mp4")
        subprocess.run([bake, "--url", trans_html, "--out", trans_out,
                        "--seconds", "3", "--fps", "30",
                        "--width", "640", "--height", "360",
                        "--clock", "seek"],
                       check=True, capture_output=True, timeout=600)

        def center_rgb(path, sec):
            # crop=2:2 而不是 1:1：yuv420p 下 1×1 会被下采样取整成 0×0。
            buf = subprocess.run(
                ["ffmpeg", "-v", "error", "-ss", str(sec), "-i", path,
                 "-frames:v", "1", "-vf", "crop=2:2:320:180",
                 "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
                capture_output=True).stdout
            return tuple(buf[:3])

        pre = center_rgb(trans_out, 0.5)
        mid = center_rgb(trans_out, 1.5)   # 后段 1.4s 起进入，窗内 0.1s
        post = center_rgb(trans_out, 2.6)
        assert pre[2] > 180 and pre[0] < 80, f"转场前应为蓝色：{pre}"
        assert post[0] > 180 and post[2] < 80, f"转场后应为红色：{post}"
        assert 40 < mid[0] < 200 and mid[2] > 40, (
            f"交叉窗内应是蓝红混色（视频层没跟 opacity 走就是纯色）：{mid}")

        print(f"hf_split_scene_smoke: PASS — 前段 RMS {rms_front:.1f}, "
              f"后段 RMS {rms_back:.1f}（错位对照 {rms_wrong:.1f}）；"
              f"接缝 fade 混色 {mid}（前 {pre} / 后 {post}）")


if __name__ == "__main__":
    main()
