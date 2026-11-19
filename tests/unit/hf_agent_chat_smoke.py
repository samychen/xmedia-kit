#!/usr/bin/env python3
# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""agent 对话链密闭冒烟（无需真实 LLM）。

用「假 agent」夹具验证 Studio agent 集成的**机械链路**——真实 LLM 质量
归 hf_agent_verify.py（需装好 CLI 的机器）：

  1. XMEDIA_AGENT_CMD → 假 agent（读 prompt → 改 spec → 打印回复）
     → _run_agent_chat 全链：spec mtime 变化、标题写回、契约页重组、
     current_spec 重载；
  2. claude JSON 解析分支：PATH 上放假 claude（输出 {"result": ...}）
     → probe 命中 → result 字段提取；
  3. 错误路径：假 agent 非零退出 → error 如实上报（不静默）；
  4. 叙事纪律注入：spec.storyboard.direction 存在时 prompt 含方向块。

真实 CLI 的登录/写权限/退出码语义归 tools/hf_agent_verify.py。
"""

import json
import os
import stat
import sys
import tempfile

TOOLS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "tools")
sys.path.insert(0, TOOLS)

os.environ.pop("LD_LIBRARY_PATH", None)

FAKE_AGENT = r'''#!/usr/bin/env python3
"""假 agent：prompt 最后一个参数 → 按 Studio 约定改 spec → 打印回复。"""
import json, re, sys
prompt = sys.argv[-1]
m = re.search(r"spec JSON 路径: (\S+)", prompt)
if not m:
    print("fake-agent: prompt 里没有 spec 路径"); sys.exit(1)
spec_path = m.group(1)
spec = json.load(open(spec_path, encoding="utf-8"))
m2 = re.search(r"把标题改成「([^」]+)」", prompt)
if m2:
    spec["scenes"][0]["text"] = m2.group(1)
json.dump(spec, open(spec_path, "w", encoding="utf-8"),
          ensure_ascii=False, indent=1)
print(f"fake-agent: 已把标题改为「{spec['scenes'][0]['text']}」")
'''


def write_exec(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)
    os.chmod(path, os.stat(path).st_mode | stat.S_IEXEC)


def main():
    import hf_studio

    tmp = tempfile.mkdtemp(prefix="hf_agent_chat_smoke_")
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))
        print(f"  {'✓' if ok else '✗'} {name}" + (f"——{detail}" if detail else ""))

    # ---------- 夹具：gallery spec → 契约页 ----------
    run = lambda *a: subprocess_run(list(a))
    gallery = os.path.join(os.path.dirname(TOOLS), "gallery",
                           "knowledge-cards.json")
    contract = os.path.join(tmp, "contract.html")
    run(sys.executable, os.path.join(TOOLS, "hf_compose.py"),
        gallery, "-o", contract)
    spec_path = os.path.join(tmp, "spec.json")
    with open(gallery, encoding="utf-8") as f:
        spec = json.load(f)
    with open(spec_path, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
        f.write("\n")

    # ---------- 1. XMEDIA_AGENT_CMD → 假 agent 全链 ----------
    fake = os.path.join(tmp, "fake_agent.py")
    write_exec(fake, FAKE_AGENT)
    os.environ["XMEDIA_AGENT_CMD"] = json.dumps(
        [sys.executable.replace("/bin:", "") if False else sys.executable,
         fake])
    srv = hf_studio.StudioServer(contract, spec_path, 0, no_sfx=True)
    title_before = str(spec["scenes"][0]["text"])
    target = "smoke 改写后的标题"
    mtime_before = os.path.getmtime(spec_path)
    contract_mtime_before = os.path.getmtime(contract)

    out = srv._run_agent_chat(f"把标题改成「{target}」，其他场景不动")
    spec_after = json.load(open(spec_path, encoding="utf-8"))
    title_after = str(spec_after["scenes"][0]["text"])
    mtime_after = os.path.getmtime(spec_path)
    contract_after = os.path.getmtime(contract)

    probe1 = srv._agent_probe()
    check("1. XMEDIA_AGENT_CMD 探测命中（custom 路径 + 显示名=可执行名）",
          probe1.get("custom") is True and probe1.get("cli") == "python3",
          str(probe1.get("cmd", [])[:2]))
    check("2. 写文件能力（spec mtime 变化）", mtime_after > mtime_before)
    check("3. 改写命中目标（标题写回）", title_after == target,
          f"{title_before!r} → {title_after!r}")
    check("4. 重组链（契约页 mtime 刷新）", contract_after > contract_mtime_before)
    check("5. current_spec 重载（Studio 内存态同步）",
          str((srv.current_spec.get("scenes") or [{}])[0]
              .get("text", "")) == target)
    check("6. 回复文本非空", "fake-agent" in str(out.get("reply", "")))
    check("7. spec_updated 标志", out.get("spec_updated") is True)

    # ---------- 2. claude JSON 解析分支 ----------
    fake_claude = os.path.join(tmp, "bin", "claude")
    os.makedirs(os.path.dirname(fake_claude), exist_ok=True)
    write_exec(fake_claude, '''#!/usr/bin/env python3
import json
print(json.dumps({"result": "claude 假回复：已完成", "is_error": False}))
''')
    saved_path = os.environ.get("PATH", "")
    os.environ["PATH"] = os.path.dirname(fake_claude) + os.pathsep + saved_path
    os.environ.pop("XMEDIA_AGENT_CMD", None)
    probe = hf_studio.StudioServer._agent_probe()
    check("8. 内置 claude 探测命中", probe.get("cli") == "claude")
    out2 = srv._run_agent_chat("随便说一句")
    os.environ["PATH"] = saved_path
    check("9. claude JSON result 字段提取",
          out2.get("reply") == "claude 假回复：已完成" and
          not out2.get("error"), str(out2.get("reply", ""))[:60])

    # ---------- 3. 错误路径：非零退出如实上报 ----------
    os.environ["XMEDIA_AGENT_CMD"] = json.dumps(
        [sys.executable, "-c", "import sys; print('boom'); sys.exit(3)"])
    out3 = srv._run_agent_chat("触发失败")
    check("10. 非零退出如实上报（不静默）",
          out3.get("error") == "exit 3" and "boom" in str(out3.get("reply", "")),
          str(out3.get("reply", ""))[:60])

    # ---------- 4. 叙事纪律注入 ----------
    spec["storyboard"] = {"arc": "launch", "message": "测试",
                          "direction": {"two_color": {"ground": "#0a0a1a",
                                                      "ink": "#f0f0f5"},
                                        "breather": 1,
                                        "banned": ["randomness"]}}
    with open(spec_path, "w", encoding="utf-8") as f:
        json.dump(spec, f, ensure_ascii=False, indent=1)
    srv.current_spec = spec
    captured = {}
    orig_run = hf_studio.subprocess.run
    def spy(cmd, **kw):
        captured["prompt"] = cmd[-1] if cmd else ""
        return orig_run(cmd, **kw)
    hf_studio.subprocess.run = spy
    try:
        srv._run_agent_chat("再改一次标题")
    finally:
        hf_studio.subprocess.run = orig_run
    prompt = captured.get("prompt", "")
    check("11. 叙事纪律注入 agent prompt",
          "叙事纪律" in prompt and "两色纪律" in prompt and
          "randomness" in prompt and "breather" in prompt)

    # ---------- 结论 ----------
    failed = [n for n, ok, _ in checks if not ok]
    print()
    if failed:
        print(f"hf_agent_chat_smoke: FAIL——{failed}")
        sys.exit(1)
    print("hf_agent_chat_smoke: ALL PASS"
          "（机械链路全通；真实 LLM 质量归 hf_agent_verify.py）")


def subprocess_run(cmd, **kw):
    import subprocess
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


if __name__ == "__main__":
    main()
