#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""叙事计划（Storyboard）→ spec 骨架（HF Storyboard 工作流的本地版）。

HF 的 Storyboard 不是场景模板库，是**商业叙事的结构模板**：先给叙事计划
（Arc / 方向块 / 每帧任务），build 再逐帧落地。本模块把这套纪律落成
确定性管线：

  1. Arc 库：商业视频的节拍骨架（launch / problem-solution / listicle /
     explainer / comparison），每帧带 type（商业任务）+ persuasion（修辞）
     + beat（情绪节拍）——HF 的叙事词汇表；
  2. 生成：选 Arc + message/audience/mood → spec 骨架（场景类型按帧任务
     映射、文案占位按 message 推导）+ spec.storyboard 段（Studio 分镜
     面板显示叙事徽标）；
  3. 方向块纪律：两色（ground/ink）进 spec 配色，负面清单进 storyboard
     段（供 agent 模式读）。

与 launch 子命令的关系：launch 是单条 Arc（Hook/Proof/Turn/CTA）的手工
填空；storyboard 是 Arc 库 + 叙事计划的完整形态。

单位约定（与仓库一致）：一切时间都是**秒**。
"""

import json
import os
import re

# ---------------------------------------------------------------------------
# Arc 商业叙事模板库：每帧 = HF 叙事词汇（type/persuasion/beat）+ 本仓库
# 的场景类型映射（hook→title、benefit_highlight→stats、social_proof→quote、
# cta→outro……）。scene 是默认场景类型，文案由生成器按帧任务推导占位。
# ---------------------------------------------------------------------------

ARCS = {
    "launch": {
        "label": "产品发布（Hook → 亮点 → 证明 → CTA）",
        "message_hint": "一句话论点：这个产品给用户什么",
        "frames": [
            {"type": "hook", "persuasion": "counterexample",
             "beat": "recognition + tension", "scene": "title",
             "duration": 3.0,
             "placeholder": "还在被 {pain} 拖慢吗？"},
            {"type": "benefit_highlight", "persuasion": "numbered enumeration",
             "beat": "aha", "scene": "stats",
             "duration": 3.5,
             "placeholder": "{proof}|核心指标说话"},
            {"type": "benefit_highlight", "persuasion": "numbered enumeration",
             "beat": "aha", "scene": "list",
             "duration": 4.0,
             "placeholder": "三件事它替你做了"},
            {"type": "cta", "persuasion": "callback + distillation",
             "beat": "resolve + inevitability", "scene": "outro",
             "duration": 2.5,
             "placeholder": "立即开始"},
        ],
    },
    "problem-solution": {
        "label": "问题-方案（Hook → 问题 → 方案 → 证明 → CTA）",
        "message_hint": "一句话论点：你如何消灭这个痛点",
        "frames": [
            {"type": "hook", "persuasion": "counterexample",
             "beat": "recognition + tension", "scene": "title",
             "duration": 3.0,
             "placeholder": "这个问题你一定遇到过"},
            {"type": "problem", "persuasion": "before/after",
             "beat": "recognition + tension", "scene": "text",
             "duration": 3.5,
             "placeholder": "旧办法的代价"},
            {"type": "solution", "persuasion": "counterexample",
             "beat": "aha", "scene": "text",
             "duration": 4.0,
             "placeholder": "新办法的一句话"},
            {"type": "social_proof", "persuasion": "numbered enumeration",
             "beat": "resolve", "scene": "quote",
             "duration": 3.5,
             "placeholder": "用户怎么说"},
            {"type": "cta", "persuasion": "callback + distillation",
             "beat": "resolve + inevitability", "scene": "outro",
             "duration": 2.5,
             "placeholder": "现在就换"},
        ],
    },
    "listicle": {
        "label": "清单体（Hook → 条目×3 → CTA）",
        "message_hint": "一句话论点：这份清单证明什么",
        "frames": [
            {"type": "hook", "persuasion": "numbered enumeration",
             "beat": "recognition", "scene": "title",
             "duration": 3.0,
             "placeholder": "三个理由"},
            {"type": "benefit_highlight", "persuasion": "numbered enumeration",
             "beat": "aha", "scene": "list",
             "duration": 3.0,
             "placeholder": "理由一"},
            {"type": "benefit_highlight", "persuasion": "numbered enumeration",
             "beat": "aha", "scene": "list",
             "duration": 3.0,
             "placeholder": "理由二"},
            {"type": "benefit_highlight", "persuasion": "numbered enumeration",
             "beat": "aha", "scene": "list",
             "duration": 3.0,
             "placeholder": "理由三"},
            {"type": "cta", "persuasion": "callback + distillation",
             "beat": "resolve + inevitability", "scene": "outro",
             "duration": 2.5,
             "placeholder": "看完就试"},
        ],
    },
    "explainer": {
        "label": "讲解（What → Why → How → 示例 → CTA）",
        "message_hint": "一句话论点：讲清楚什么",
        "frames": [
            {"type": "hook", "persuasion": "counterexample",
             "beat": "recognition", "scene": "title",
             "duration": 3.0,
             "placeholder": "这是什么"},
            {"type": "benefit_highlight", "persuasion": "before/after",
             "beat": "aha", "scene": "text",
             "duration": 3.5,
             "placeholder": "为什么重要"},
            {"type": "solution", "persuasion": "numbered enumeration",
             "beat": "aha", "scene": "list",
             "duration": 4.0,
             "placeholder": "怎么用"},
            {"type": "social_proof", "persuasion": "before/after",
             "beat": "resolve", "scene": "text",
             "duration": 3.5,
             "placeholder": "实际效果"},
            {"type": "cta", "persuasion": "callback + distillation",
             "beat": "resolve + inevitability", "scene": "outro",
             "duration": 2.5,
             "placeholder": "上手试试"},
        ],
    },
    "comparison": {
        "label": "对比（Hook → 之前 → 之后 → 证明 → CTA）",
        "message_hint": "一句话论点：对比之后你选什么",
        "frames": [
            {"type": "hook", "persuasion": "before/after",
             "beat": "recognition + tension", "scene": "title",
             "duration": 3.0,
             "placeholder": "两种做法"},
            {"type": "problem", "persuasion": "before/after",
             "beat": "recognition + tension", "scene": "comparison",
             "duration": 4.0,
             "placeholder": "之前|繁琐又慢"},
            {"type": "solution", "persuasion": "before/after",
             "beat": "aha", "scene": "comparison",
             "duration": 4.0,
             "placeholder": "之后|一条命令"},
            {"type": "social_proof", "persuasion": "numbered enumeration",
             "beat": "resolve", "scene": "stats",
             "duration": 3.0,
             "placeholder": "90%|团队已切换"},
            {"type": "cta", "persuasion": "callback + distillation",
             "beat": "resolve + inevitability", "scene": "outro",
             "duration": 2.5,
             "placeholder": "选更好的那个"},
        ],
    },
}


def arcs():
    """Arc 清单 → [{id, label, frames: N}]（CLI list 用）。"""
    return [{"id": k, "label": v["label"],
             "frames": len(v["frames"])} for k, v in ARCS.items()]


def _fill_placeholder(text, message):
    """占位文案填 message 的关键词；无关键词回退 message 本身。"""
    kw = ""
    m = re.search(r"[「'\"]([^」'\"]{2,12})[」'\"]", message or "")
    if m:
        kw = m.group(1)
    out = text.replace("{pain}", kw or "旧流程").replace(
        "{proof}", kw or "100%")
    if "{pain}" in out or "{proof}" in out:
        out = out.replace("{pain}", "").replace("{proof}", "")
    return out.strip() or (message or "").strip() or "待填文案"


def build(arc, message, audience="", mood="", fmt="9:16", fps=30,
          voice="Tingting"):
    """Arc + 叙事计划 → 完整 spec（含 storyboard 段）。

    storyboard.frames 与 spec.scenes 一一对应（同索引）——Studio 分镜
    面板按索引取叙事徽标。direction.two_color 进场景配色（ground 为
    全局背景、ink 为主文字色），负面清单留 storyboard.direction.banned。
    """
    arc_def = ARCS[arc]
    scenes, frame_records = [], []
    palette = ["#1a1a2e", "#16213e", "#0f3460", "#1a1a2e", "#16213e"]
    for i, fr in enumerate(arc_def["frames"]):
        sc = {
            "type": fr["scene"],
            "text": _fill_placeholder(fr["placeholder"], message),
            "bg": palette[i % len(palette)],
            "duration": fr["duration"],
            "animation": "fade_in_up" if fr["type"] == "hook" else "fade_in",
        }
        if fr["scene"] == "list":
            # list 帧补副行占位（frame-worker 语言：主行 + 支撑注释行）
            # ——骨架画面不至一行孤字，用户/agent 有明确的填空位。
            sc["subtitle"] = "这一条为什么成立——一句话支撑，或换成具体数字"
        scenes.append(sc)
        frame_records.append({
            "idx": i, "type": fr["type"],
            "persuasion": fr["persuasion"], "beat": fr["beat"],
            "note": "",
        })
    # breather：默认最长的帧（HF：全片一个刻意更静的节拍）
    breather = max(range(len(scenes)),
                   key=lambda i: scenes[i]["duration"])
    spec = {
        "composition": {"id": arc + "-storyboard", "format": fmt,
                        "fps": fps},
        "background": "#0a0a1a",
        "tts": {"provider": "say", "voice": voice, "rate": 200},
        "scenes": scenes,
        "storyboard": {
            "arc": arc,
            "message": message or "",
            "audience": audience or "",
            "mood": mood or "",
            "direction": {
                "two_color": {"ground": "#0a0a1a", "ink": "#f0f0f5"},
                "reveal": "vo",
                "breather": breather,
                "banned": ["purple-blue AI gradients", "bokeh",
                           "browser chrome", "drop-shadow cards",
                           "infinite loops", "randomness"],
            },
            "frames": frame_records,
        },
    }
    return spec


def main():
    import argparse
    p = argparse.ArgumentParser(description="叙事计划 → spec（调试用，"
                                "正式入口 xmedia.py storyboard）")
    p.add_argument("arc")
    p.add_argument("--message", default="")
    p.add_argument("-o", default="storyboard.json")
    a = p.parse_args()
    spec = build(a.arc, a.message)
    with open(a.o, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
    print("wrote", a.o)


if __name__ == "__main__":
    main()
