#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""契约页的只读解析：场景、音轨、总时长、预览预混音。

这些函数只吃 HTML 文本、只吐数据结构，不碰 HTTP、不碰 Studio 的会话状态，
也不写任何文件（预混音除外，它写临时 WAV 再读回内存）。拆出本模块是为了
让 hf_studio.py 专注服务与注入：那两件事各占一半篇幅，而契约解析是一条
独立的、可单独测的缝。

为什么不并进 hf_compose.py：那个模块是**生成**契约页的，本模块是**读回**
契约页的。生成侧不该依赖读回侧，否则「compose 一个页面」会连带拖进正则
解析与 ffmpeg 调用。

单位约定（与采集端一致，写错了画面/声音会静悄悄地错一段）：
  * data-start / data-duration / data-media-start 都是**秒**；
  * 返回结构里的 start / dur / media_start 同样是秒；
  * 只有 html_bake 的采集端才会把秒乘 1000。
"""

import os
import re
import subprocess
import tempfile
import urllib.parse

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TOOLS_DIR)

AUDIO_EXTS = {".wav", ".mp3", ".m4a", ".aac"}
# 服务端预混音的路由；契约页本身不含它，只在 /preview 的内存副本里注入。
PREVIEW_MIX_URL = "/__xmedia_preview_mix__.wav"

# 下面两个前缀是 /preview 内存副本里的改写结果，解析音轨时要把它们映射
# 回本地路径——否则预览混音读到的是 "/__xmedia_root__/assets/sfx/x.wav"
# 这种 URL，ffmpeg 找不到文件，整条混音降级（预览又变成逐条 play()）。
ROOT_ASSET_PREFIX = "/__xmedia_root__/"
EXTERNAL_ASSET_PREFIX = "/__xmedia_media__/"


def parse_scenes(html):
    """契约页的场景清单：div 且同时带 data-start/data-duration——合成根
    （data-composition-id）是容器不是场景，排除。
    返回 [{id, start, dur, label}]（秒）。"""
    scenes = []
    for m in re.finditer(r'<div\b([^>]*)>', html, re.S):
        attrs = m.group(1)
        if "data-composition-id" in attrs:
            continue
        start = re.search(r'data-start="([\d.]+)"', attrs)
        dur = re.search(r'data-duration="([\d.]+)"', attrs)
        sid_m = re.search(r'id="([^"]+)"', attrs)
        cls_m = re.search(r'class="([^"]*)"', attrs)
        if not (start and dur and sid_m):
            continue
        sid = sid_m.group(1)
        cls = cls_m.group(1) if cls_m else ""
        typ = next((c for c in cls.split() if c.startswith("scene-")
                    and c != "scene"), sid)
        scenes.append({"id": sid, "start": float(start.group(1)),
                       "dur": float(dur.group(1)), "label": typ})
    return scenes


def parse_audio_tracks(html):
    """页面 <audio>/<video> 元素 → 音轨清单（秒）。

    [{src, start, dur, vol, media_start, kind}]
    kind：narration（voice/ 旁白）/ sfx（assets/sfx）/ bgm（其余）/
    video（<video> 素材自带音轨）。

    <video> 必须一起进混音：成片的声音本来就包含实拍素材的原声，导出侧
    （html_bake 的 is_media 清单）一直按场景窗口把它叠进去；预览若只认
    <audio>，导入素材的工程在 Studio 里就是全静音（画面对、完全没声）。
    video 轨道不进音轨车道——声音属于片段，不是独立音轨。
    """
    tracks = []
    for m in re.finditer(r'<(audio|video)\b([^>]*)>', html, re.S):
        tag, attrs = m.group(1), m.group(2)
        # 显式关闭原声（data-has-audio="false"）不参与混音
        if tag == "video" and 'data-has-audio="false"' in attrs:
            continue
        src = re.search(r'src="([^"]+)"', attrs)
        start = re.search(r'data-start="([\d.]+)"', attrs)
        dur = re.search(r'data-duration="([\d.]+)"', attrs)
        vol = re.search(r'data-volume="([\d.]+)"', attrs)
        if not (src and start):
            continue
        s = src.group(1)
        if tag == "video":
            kind = "video"
        else:
            kind = ("sfx" if "assets/sfx" in s else
                    "narration" if ("voice/" in s or "narr" in
                                    os.path.basename(s)) else "bgm")
        # 源内起点（片段切割）：音轨从素材的第 media_start 秒起播，
        # 与画面 data-media-start 同源，否则切过的工程声音会提前一段。
        ms = re.search(r'data-media-start="([\d.]+)"', attrs)
        tracks.append({"src": s, "start": float(start.group(1)),
                       "dur": float(dur.group(1)) if dur else 0.0,
                       "vol": float(vol.group(1)) if vol else 1.0,
                       "media_start": float(ms.group(1)) if ms else 0.0,
                       "kind": kind})
    return tracks


def parse_total(html, scenes):
    """总时长（秒）：合成根 data-duration 优先，回退场景窗最大 end。"""
    m = re.search(r'data-composition-id[^>]*data-duration="([\d.]+)"',
                  html, re.S)
    if m:
        return float(m.group(1))
    return max((s["start"] + s["dur"] for s in scenes), default=0.0)


def _track_local_path(src, contract_path):
    """契约页音轨 src → 本地文件绝对路径（含 ROOT_ASSET_PREFIX 映射）。"""
    p = urllib.parse.unquote(src)
    if p.startswith(EXTERNAL_ASSET_PREFIX):
        p = "/" + p[len(EXTERNAL_ASSET_PREFIX):]
    elif p.startswith(ROOT_ASSET_PREFIX):
        p = os.path.join(ROOT_DIR, p[len(ROOT_ASSET_PREFIX):])
    elif not os.path.isabs(p):
        p = os.path.join(os.path.dirname(contract_path), p)
    return os.path.realpath(p)


def build_preview_mix(contract_path, html, total_s):
    """把契约页的多条 <audio> / <video> 预混成单条 WAV（仅服务模式预览）。

    播放中途不再有任何 HTMLAudioElement 首次 play()：Safari 的音轨解码器 /
    输出设备初始化是同步的，会阻塞与父页共享的主线程，表现为第二个、第三个
    音效起点前游标突然停住数帧。混音在服务端按 data-start / data-volume /
    data-media-start 做一次，契约页、注入模式的单文件、file:// 烘焙链路
    全部不变。

    <video> 也在这条链上：视频元素本身带 muted（预览画由它自己播，声音统一
    走混音），素材原声由此叠进单轨——否则导入素材的工程预览完全没有声音。
    """
    tracks = parse_audio_tracks(html)
    pairs = []
    for t in tracks:
        p = _track_local_path(t["src"], contract_path)
        if os.path.isfile(p):
            pairs.append((p, t))
    if not pairs or total_s <= 0:
        return None
    cmd = ["ffmpeg", "-v", "error", "-y"]
    filt = []
    for i, (p, t) in enumerate(pairs):
        # 源内起点（data-media-start）：-ss 放在 -i 之前，只解音轨且直接
        # 跳到目标位置——20Mbps 的实拍素材重解一遍画面会让预览起播卡住。
        ms = max(0.0, float(t.get("media_start") or 0.0))
        if ms > 0:
            cmd += ["-ss", f"{ms:.3f}"]
        cmd += ["-i", p]
        delay = max(0, int(round(t["start"] * 1000)))
        vol = max(0.0, min(4.0, float(t["vol"])))
        dur = max(0.0, float(t.get("dur") or 0.0))
        # 按片段窗口修剪：不加 atrim 时，切过的后段音轨会盖过场景终点
        # 继续向前播（画面已走，声音还在）。asetpts 重置时间戳，否则
        # atrim 保留的 PTS 会把 adelay 的起点整体推后。
        chain = (f"[{i}:a]aformat=sample_rates=48000:channel_layouts=stereo,")
        if dur > 0:
            chain += f"atrim=0:{dur:.3f},asetpts=PTS-STARTPTS,"
        chain += f"adelay={delay}:all=1,volume={vol:.4f}[a{i}]"
        filt.append(chain)
    ins = "".join(f"[a{i}]" for i in range(len(pairs)))
    # 尾部不补 apad：amix 的输入被 atrim 截断后，apad 的无限静音源与
    # adelay 的 EOF 推迟在 ffmpeg 7 上会让 framesync 永远等不到结束
    # （实测进程挂死，输出文件停在半截）。改在输出端 -t 总时长收口：
    # 末轨之后的静音本来就不需要写进 WAV，播放器播到结尾即可。
    filt.append(
        f"{ins}amix=inputs={len(pairs)}:duration=longest:normalize=0[out]")
    cmd += ["-filter_complex", ";".join(filt), "-map", "[out]",
            "-t", f"{total_s:.3f}",
            "-ac", "2", "-ar", "48000", "-c:a", "pcm_s16le"]
    fd, tmp = tempfile.mkstemp(prefix="xmedia_preview_", suffix=".wav")
    os.close(fd)
    try:
        cmd += ["-f", "wav", tmp]
        # 环境净化：托管运行时注入的 LD_LIBRARY_PATH 与系统 ffmpeg 的
        # 插件依赖（GLIBCXX 版本）冲突——不剥掉则预混音全数失败（与
        # hf_studio._ff_env / hf_reference 同根因）。
        env = os.environ.copy()
        env.pop("LD_LIBRARY_PATH", None)
        proc = subprocess.run(cmd, capture_output=True, timeout=60, env=env)
        if proc.returncode != 0 or os.path.getsize(tmp) < 1024:
            return None
        with open(tmp, "rb") as f:
            return f.read()
    except (OSError, subprocess.SubprocessError):
        return None  # ffmpeg 缺失 / 素材异常 → 自动降级到多轨预览引擎
    finally:
        try:
            os.unlink(tmp)
        except OSError:
            pass
