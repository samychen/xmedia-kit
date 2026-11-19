#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# launch skill 的 gate：底线检查，格式对齐 HF（FAIL <rule>: <what>，
# 末行 RESULT: PASS/FAIL，失败 exit 非零）。
# 用法：python3 skills/launch/scripts/gate.py <项目目录>
import json
import os
import re
import subprocess
import sys

FAILS = []
OKS = []


def check(rule, ok, detail=""):
    (OKS if ok else FAILS).append((rule, detail))
    return ok


def main(project_dir):
    project_dir = os.path.abspath(project_dir or ".")
    spec_file = os.path.join(project_dir, "spec.json")
    html_file = os.path.join(project_dir, "contract.html")
    check("layout: 项目文件", os.path.isfile(spec_file) and os.path.isfile(html_file),
          "缺 spec.json 或 contract.html 于 " + project_dir)
    if FAILS:
        _result()
        return 1
    spec = json.load(open(spec_file, encoding="utf-8"))

    check("schema_version", spec.get("schema_version") == "1.0",
          str(spec.get("schema_version")))
    scenes = spec.get("scenes") or []
    check("scenes_nonempty", bool(scenes), f"{len(scenes)} 个")

    sb = spec.get("storyboard") or {}
    frames = sb.get("frames") or []
    check("storyboard_arc", bool(sb.get("arc")), "缺 storyboard.arc")
    check("storyboard_frames_same_len", len(frames) == len(scenes),
          f"frames {len(frames)} vs scenes {len(scenes)}")

    placeholders = re.compile(r"\[slot\]|占位|待填|placeholder",
                              re.I)
    dirty = []
    for i, sc in enumerate(scenes):
        if not str(sc.get("text", "")).strip():
            FAILS.append((f"scene_text_{i+1}", "空 text"))
            dirty.append(True)
        for v in (json.dumps(sc, ensure_ascii=False)):
            pass
        blob = json.dumps(sc, ensure_ascii=False)
        if placeholders.search(blob) and "[slot]" in blob:
            dirty.append(True)
    check("no_placeholder_slots", not any(dirty),
          "存在未填的模板 slot 或空文案")

    durs = [float(sc.get("duration", 3)) for sc in scenes]
    check("durations_valid", all(0.5 <= d <= 60 for d in durs),
          ", ".join(str(d) for d in durs))

    has_audio = bool((spec.get("scenes") or [{}])[0].get("narration")
                     or spec.get("audio"))
    silent_ok = os.path.isfile(os.path.join(project_dir, ".silent"))
    check("audio_or_silent", has_audio or silent_ok,
          "无旁白/BGM 且未声明静音（写 .silent 声明显式静音）")

    proc = subprocess.run(
        [sys.executable,
         os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "..", "..", "tools", "hf_validate.py"),
         html_file],
        capture_output=True, text=True, timeout=120)
    check("hf_validate_0_errors", proc.returncode == 0,
          (proc.stdout or proc.stderr)[-200:])

    _result()
    return 1 if FAILS else 0


def _result():
    for rule, detail in FAILS:
        print(f"FAIL {rule}: {detail}")
    for rule, detail in OKS:
        print(f"OK   {rule}")
    print("RESULT: " + ("FAIL" if FAILS else "PASS"))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
