#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""规则引擎指令单测（dcbdc63 的 8 类高频编辑指令 + 核心回归 + 负向用例）。

hf_agent.handle(message, spec, spec_dir) 是纯函数（rules → spec 变更），
逐条断言「指令 → spec 字段」的映射——规则引擎是 chat 主界面的零配置
后端（HF Studio agent 的本地兜底），它的每条指令都必须确定：

  1. 副标题逐场景改   2. 单场景背景色   3. 单场景文字色
  4. 场景级转场（白名单）  5. 入场动画（白名单）
  6. 指定位置插入场景  7. 模板场景插入（temerrates/ 热加载）
  8. BGM 音量（0-1 钳制）

负向：白名单外值不改 spec、模板不存在给 help、越界索引给 help。
"""

import copy
import json
import os
import sys

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "tools")
sys.path.insert(0, TOOLS)

os.environ.pop("LD_LIBRARY_PATH", None)

import hf_agent  # noqa: E402

BASE_SPEC = {
    "schema_version": "1.0",
    "composition": {"id": "rules-smoke", "format": "16:9", "fps": 30},
    "background": "#0a0a1a",
    "scenes": [
        {"type": "title", "text": "标题一", "subtitle": "副标题一",
         "duration": 3, "bg": "#1a1a2e"},
        {"type": "text", "text": "正文二", "duration": 3, "bg": "#16213e"},
        {"type": "outro", "text": "结束三", "duration": 2, "bg": "#0f3460"},
    ],
}

checks = []


def check(name, ok, detail=""):
    checks.append((name, bool(ok), detail))
    print(f"  {'✓' if ok else '✗'} {name}" + (f"——{detail}" if detail else ""))


def run(msg, spec=None, spec_dir=""):
    return hf_agent.handle(msg, copy.deepcopy(spec or BASE_SPEC),
                           spec_dir=spec_dir)


def main():
    # ---------- 1. 副标题逐场景改 ----------
    r = run("第 2 个场景副标题改成：新的副标题")
    check("1a. 副标题定位到第 2 场景", r.get("spec") and
          r["spec"]["scenes"][1].get("subtitle") == "新的副标题")
    r = run("第 1 个场景副标题清空")
    check("1b. 副标题清空", r.get("spec") and
          r["spec"]["scenes"][0].get("subtitle") == "")

    # ---------- 2. 单场景背景色 ----------
    r = run("第 3 个场景背景 #ff0000")
    check("2. 单场景背景色（不动全局）", r.get("spec") and
          r["spec"]["scenes"][2]["bg"] == "#ff0000" and
          r["spec"]["background"] == "#0a0a1a")

    # ---------- 3. 单场景文字色 ----------
    r = run("第 1 个场景文字色 #00ff88")
    check("3. 单场景文字色", r.get("spec") and
          r["spec"]["scenes"][0].get("text_color") == "#00ff88")

    # ---------- 4. 场景级转场（白名单） ----------
    r = run("第 2 个场景转场改成 slide-left")
    check("4a. 转场白名单值生效", r.get("spec") and
          r["spec"]["scenes"][1].get("transition") == "slide-left")
    r = run("第 2 个场景转场改成 dissolve")
    unchanged = (r.get("spec") is None or
                 r["spec"]["scenes"][1].get("transition") is None)
    check("4b. 转场白名单外值不改 spec", unchanged,
          str(r.get("reply", ""))[:50])

    # ---------- 5. 入场动画（白名单） ----------
    r = run("第 1 个场景动画改成 scale_in")
    check("5a. 动画白名单值生效", r.get("spec") and
          r["spec"]["scenes"][0].get("animation") == "scale_in")
    r = run("第 1 个场景动画改成 spin")
    unchanged = (r.get("spec") is None or
                 r["spec"]["scenes"][0].get("animation") is None)
    check("5b. 动画白名单外值不改 spec", unchanged)

    # ---------- 6. 指定位置插入场景 ----------
    r = run("在第 2 个场景之前插入场景，文字：插进来的")
    spec = r.get("spec") or {}
    ok = (len(spec.get("scenes", [])) == 4 and
          spec["scenes"][1]["text"] == "插进来的" and
          spec["scenes"][2]["text"] == "正文二")
    check("6a. 指定位置插入（第 2 位前 + 原场景顺延）", ok,
          f"{len(spec.get('scenes', []))} 场景")
    r = run("在第 99 个场景之前插入场景，文字：越界")
    spec = r.get("spec") or {}
    check("6b. 越界位置钳制到末尾", spec and
          spec["scenes"][-1]["text"] == "越界")

    # ---------- 7. 模板场景插入（templates/ 热加载） ----------
    repo_templates = os.path.join(os.path.dirname(TOOLS), "templates")
    r = run("加一个 hf-counter-burst 模板场景", spec_dir=repo_templates)
    spec = r.get("spec") or {}
    last = (spec.get("scenes") or [{}])[-1]
    check("7a. 模板场景追加（type + slot 占位）",
          len(spec.get("scenes", [])) == 4 and
          last.get("type") == "hf-counter-burst" and
          any(str(v).startswith("[") for k, v in last.items()
              if k not in ("type", "duration")),
          json.dumps(last, ensure_ascii=False)[:80])
    r = run("加一个 不存在的模板场景", spec_dir=repo_templates)
    check("7b. 模板不存在 → help（不改 spec）",
          r.get("spec") is None and r.get("mode") == "help" and
          "不存在" in str(r.get("reply", "")), str(r.get("reply", ""))[:40])

    # ---------- 8. BGM 音量（0-1 钳制） ----------
    r = run("BGM 音量 0.5")
    check("8a. BGM 音量设置", r.get("spec") and
          (r["spec"].get("audio", {}).get("bgm", {})
           .get("volume")) == 0.5)
    r = run("BGM 音量 5")
    check("8b. 音量上钳制 1.0", r.get("spec") and
          r["spec"]["audio"]["bgm"]["volume"] == 1.0)

    # ---------- 核心回归（既有指令不被新分支抢匹配） ----------
    r = run("第 1 个场景文字改成 新正文")
    check("9a. 场景文字改（不是副标题分支）", r.get("spec") and
          r["spec"]["scenes"][0]["text"] == "新正文" and
          r["spec"]["scenes"][0].get("subtitle") == "副标题一")
    r = run("改成竖屏")
    check("9b. 画幅 9:16", r.get("spec") and
          r["spec"]["composition"]["format"] == "9:16")
    r = run("关闭字幕")
    check("9c. 关闭字幕轨", r.get("spec") and
          r["spec"].get("captions", {}).get("enabled") is False)

    # ---------- 结论 ----------
    failed = [n for n, ok, _ in checks if not ok]
    print()
    if failed:
        print(f"hf_agent_rules_smoke: FAIL——{failed}")
        sys.exit(1)
    print(f"hf_agent_rules_smoke: ALL PASS（{len(checks)} 项）")


if __name__ == "__main__":
    main()
