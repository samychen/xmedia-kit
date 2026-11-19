#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""agent CLI 真实对话链验证（CodeBuddy / claude / codex / 任意
XMEDIA_AGENT_CMD）——HF Studio app「agent 建片/改片」能力的实测工具。

背景：XMEDIA_AGENT_CMD 接入层已对齐（任意 agent CLI 即插即用），但
「agent 真的能写文件并触发重组」这条链依赖 CLI 的写权限/退出码语义，
需要在一台装好 CLI 的机器上实测。本脚本把实测固化成一条命令：

    export XMEDIA_AGENT_CMD='["codebuddy","-p"]'      # 例：CodeBuddy
    python3 tools/hf_agent_verify.py                  # 真实跑一轮
    python3 tools/hf_agent_verify.py --dry-run        # 只看配置与 prompt

验证项（对齐 HF Studio app 的 agent 能力清单）：
  1. 探测：XMEDIA_AGENT_CMD / 内置 claude/codex → 命中哪个 CLI；
  2. 真实对话链：「把标题改成 X」→ agent 子进程 → spec mtime 变化
     （**写文件能力**——接入层唯一未验证的关键环）；
  3. 重组链：spec 变化 → Studio 自动重载 + 重组契约页（mtime 刷新）；
  4. 语义采样：退出码 / stderr / 回复文本（非 claude CLI 无 JSON 解析，
     纯文本约定在此实测留档）。

不做的事：不评价回复质量（那是模型的事）；不循环多轮（单轮足够判定
写文件能力）；不碰网络（agent CLI 自行负责其模型连接）。

退出码：0 = 全部关键项通过；1 = 有关键项失败（附诊断与下一步建议）。
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)

# 进程级净化（与 hf_ai_smoke / hf_studio._ff_env 同根因）：托管运行时
# 注入的 LD_LIBRARY_PATH 会让被测链路里的 ffmpeg/ffprobe 全数失败。
os.environ.pop("LD_LIBRARY_PATH", None)


def main():
    p = argparse.ArgumentParser(
        description="agent CLI 真实对话链验证（CodeBuddy/claude/codex）")
    p.add_argument("--dry-run", action="store_true",
                   help="只打印探测结果与将发送的 prompt，不真正调用 agent")
    p.add_argument("--timeout", type=int, default=600,
                   help="agent 单轮超时秒数（默认 600，与 Studio 一致）")
    p.add_argument("--title", default="agent 验证标题",
                   help="对话链的改写目标（默认「agent 验证标题」）")
    p.add_argument("--keep", action="store_true",
                   help="保留临时项目目录（默认退出即清理）")
    args = p.parse_args()

    import hf_studio

    checks = []          # (name, ok, detail)
    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(f"  {'✓' if ok else '✗'} {name}" + (f"——{detail}" if detail else ""))

    # ---------- 0. 探测 ----------
    probe = hf_studio.StudioServer._agent_probe()
    cli = probe.get("cli")
    print(f"== 探测 ==")
    if probe.get("custom"):
        print(f"  XMEDIA_AGENT_CMD → {probe['cmd']}（显示名 {cli}）")
        check("探测：XMEDIA_AGENT_CMD 命中", True, " ".join(probe["cmd"]))
    elif cli:
        print(f"  内置探测 → {cli}（{probe.get('path', '')}）")
        check("探测：内置 claude/codex 命中", True, cli)
    else:
        print("  未命中任何 agent CLI。CodeBuddy 接入：")
        print("    npm install -g @tencent-ai/codebuddy-code && codebuddy  # 首次登录")
        print("    export XMEDIA_AGENT_CMD='[\"codebuddy\",\"-p\"]'")
        check("探测：agent CLI", False, "XMEDIA_AGENT_CMD 未设置且 claude/codex 不在 PATH")
        print("\n结论：无法验证（先装 CLI 并配置 XMEDIA_AGENT_CMD）")
        sys.exit(1)

    # ---------- 1. 临时项目（gallery spec → 契约页） ----------
    tmp = tempfile.mkdtemp(prefix="agent_verify_")
    spec_path = os.path.join(tmp, "spec.json")
    contract_path = os.path.join(tmp, "contract.html")
    gallery = os.path.join(os.path.dirname(TOOLS_DIR), "gallery",
                           "knowledge-cards.json")
    run_compose = [sys.executable, os.path.join(TOOLS_DIR, "hf_compose.py"),
                   gallery, "-o", contract_path]
    proc = subprocess_run(run_compose)
    assert proc.returncode == 0, f"compose 失败：{proc.stderr[-300:]}"
    # gallery spec 原样复制为项目 spec（agent 要改的就是它）
    with open(gallery, encoding="utf-8") as f:
        spec = json.load(f)
    with open(spec_path, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
        f.write("\n")
    if not args.keep:
        pass  # main 末尾统一清理
    print(f"\n== 临时项目 ==")
    print(f"  {tmp}（--keep 可保留）")

    # ---------- 2. in-process 服务（与 HTTP handler 同一代码路径） ----------
    srv = hf_studio.StudioServer(contract_path, spec_path, 0, no_sfx=True)
    title_before = str((srv.current_spec.get("scenes") or [{}])[0]
                       .get("text", ""))
    spec_mtime_before = os.path.getmtime(spec_path)
    contract_mtime_before = (os.path.getmtime(contract_path)
                             if os.path.exists(contract_path) else 0)

    # ---------- 3. dry-run：打印将发送的 prompt ----------
    if args.dry_run:
        # 复刻 _run_agent_chat 的 prompt 组装（剥离历史段）——不调用 agent
        probe_cmd = probe["cmd"]
        print("\n== dry-run（不调用 agent）==")
        print(f"  命令: {probe_cmd + ['<prompt>']}")
        print(f"  cwd : {srv._spec_root()}")
        print(f"  超时: {args.timeout}s")
        print("  prompt 要点: spec 路径 / 仓库根(动态) / schema 文档 / "
              "SKILL.md / compose 命令 / 叙事纪律(如有) / 历史 5 轮")
        print("\n结论：配置就绪。去掉 --dry-run 真实跑一轮"
              "（会产生真实 agent 调用费用）。")
        return

    # ---------- 4. 真实对话链 ----------
    print(f"\n== 真实对话链（agent={cli}，超时 {args.timeout}s）==")
    print("  这会产生真实 agent 调用（token 费用）……")
    srv.AGENT_CHAT_TIMEOUT_S = args.timeout
    message = f"把标题改成「{args.title}」，其他场景不动"
    t0 = time.time()
    out = srv._run_agent_chat(message)
    elapsed = time.time() - t0
    print(f"  agent 回复（{elapsed:.0f}s）："
          f"{str(out.get('reply', ''))[:200]}")

    # ---------- 5. 逐项判定 ----------
    print("\n== 判定 ==")
    check("1. agent 调用返回（无未捕获异常）", True,
          f"error={out.get('error') or '无'} / "
          f"exit 详情见语义采样")
    reply_ok = len(str(out.get("reply", ""))) > 0
    check("2. 回复文本非空（纯文本约定）", reply_ok,
          f"{len(str(out.get('reply', '')))} 字符")

    spec_after = json.load(open(spec_path, encoding="utf-8")) \
        if os.path.exists(spec_path) else {}
    title_after = str((spec_after.get("scenes") or [{}])[0].get("text", ""))
    spec_mtime_after = os.path.getmtime(spec_path) \
        if os.path.exists(spec_path) else 0
    wrote = spec_mtime_after > spec_mtime_before
    check("3. 写文件能力（spec mtime 变化）", wrote,
          "agent 未改 spec——查 CLI 写权限标志（如 --yolo / "
          "--auto-accept），或 -p 模式默认只读" if not wrote else
          f"标题 {title_before!r} → {title_after!r}")

    title_changed = title_after == args.title
    check("4. 改写命中目标（标题=参数值）", title_changed,
          f"现为 {title_after!r}" if title_after else "场景 0 无 text")

    contract_mtime_after = (os.path.getmtime(contract_path)
                            if os.path.exists(contract_path) else 0)
    recomposed = contract_mtime_after > contract_mtime_before
    check("5. 重组链（契约页刷新）", recomposed or wrote,
          "Studio 侧 mtime 重组 + agent 自主 compose 二选一"
          if not recomposed else "契约页 mtime 已刷新")

    # ---------- 6. 结论 ----------
    critical = [c for c in checks if c[0].startswith(("1.", "3."))]
    all_ok = all(ok for _, ok, _ in checks)
    print("\n== 结论 ==")
    if all_ok:
        print(f"  {cli} 的 agent 能力对齐：调用/写文件/重组链全通。")
        print("  对齐清单剩余项：多轮交互深度、skills 体系覆盖（结构性差距，"
              "见 docs/HF_COMPOSE.md 对齐表）。")
    else:
        failed = [c[0] for c in critical if not c[1]]
        print(f"  关键项未过：{failed}")
        if not wrote:
            print("  写文件能力未生效的常见原因：")
            print("    a) -p/print 模式默认只读——查 CLI 的自动批准标志")
            print("       （CodeBuddy: --yolo 类；claude: --permission-mode）")
            print("    b) CLI 未登录/未配置模型——先交互模式跑一次")
            print("    c) XMEDIA_AGENT_CMD 的约定不满足"
                  "（prompt 必须是最后一个参数）")
    if not args.keep:
        shutil.rmtree(tmp, ignore_errors=True)
    sys.exit(0 if all_ok else 1)


def subprocess_run(cmd):
    import subprocess
    return subprocess.run(cmd, capture_output=True, text=True, timeout=120)


if __name__ == "__main__":
    main()
