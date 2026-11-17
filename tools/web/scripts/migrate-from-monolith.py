#!/usr/bin/env python3
"""一次性迁移脚本：tools/studio_ui.html → tools/web/src 分片源码。

设计决策（对齐 FRONTEND-MODERNIZATION-PLAN.md M3 修订）：
  * 原脚本用顶层 var/function 全局共享状态，且存在大量跨域重赋值
    （playing = true 等）。ESM 导入绑定不可跨模块赋值，机械转换必然
    改变行为——违反 M3 的"行为零变化"铁律。
  * 因此 M3 产物形态为：**分片源码 + 确定性拼接构建**。每个分片是原
    顶层语句的逐字节切片；构建按 build-order.json 依原行序拼接，
    产出与原文件逐字节一致的 dist/studio_ui.html（diff 为验收门）。
  * 分片间引用关系写入 fragment-deps.json，作为 M5（store + ESM）
    的迁移地图；全局符号写入 globals.json 供 eslint no-undef 放行。
  * 执行顺序 = 原行序，由 build-order.json 固定。

产出：src/styles/*.css、src/js/**/*.js、src/index.html（模板）、
scripts/build-order.json、scripts/fragment-deps.json、scripts/globals.json。
"""

import json
import os
import re

ROOT = "/home/vanta/work/dsh-wk/xmedia-kit"
SRC_HTML = os.path.join(ROOT, "tools", "studio_ui.html")
OUT = os.path.join(ROOT, "tools", "web", "src")
SCRIPTS = os.path.join(ROOT, "tools", "web", "scripts")

# ---------- 1. 读入与结构定位 ----------
text = open(SRC_HTML, encoding="utf-8").read()
lines = text.split("\n")
n = len(lines)

def find(pred, start=0):
    for i in range(start, n):
        if pred(lines[i]):
            return i
    raise SystemExit(f"marker not found after line {start}")

style_open = find(lambda l: l.strip() == "<style>")
style_close = find(lambda l: l.strip() == "</style>", style_open)
head_close = find(lambda l: l.strip() == "</head>", style_close)
body_open = find(lambda l: l.strip() == "<body>", head_close)
script_open = find(lambda l: l.strip() == "<script>", body_open)
script_close = find(lambda l: l.strip() == "</script>", script_open)
assert script_close == n - 4, f"unexpected tail: {n} vs {script_close}"

# ---------- 2. 字符串/注释/正则感知的括号深度 ----------
REGEX_PREV = set("(,=:[!&|?{};+-*%~^<>")
KEYWORD_BEFORE = {"return", "typeof", "case", "in", "of", "instanceof",
                  "new", "delete", "void", "do", "else", "yield", "await"}

def _prev_code_char(out_chars):
    for ch in reversed(out_chars):
        if not ch.isspace():
            return ch
    return None

def _prev_word(out_chars):
    word = []
    for ch in reversed(out_chars):
        if ch.isalnum() or ch in "_$":
            word.append(ch)
        elif ch.isspace():
            if word:
                break
        else:
            break
    return "".join(reversed(word))

def strip_noise(line, mode=None):
    """剥离字符串/注释/正则字面量，返回 (净代码, 结束时模式)。"""
    out, i = [], 0
    while i < len(line):
        c = line[i]
        if mode is None:
            if c in "\"'":
                mode = c
                i += 1
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                mode = "/*"
                i += 2
                continue
            if c == "/":
                prev = _prev_code_char(out)
                is_regex = (prev is None or prev in REGEX_PREV
                            or _prev_word(out) in KEYWORD_BEFORE)
                if is_regex:
                    j = i + 1
                    in_class = False
                    closed = False
                    while j < len(line):
                        ch = line[j]
                        if ch == "\\":
                            j += 2
                            continue
                        if ch == "[":
                            in_class = True
                        elif ch == "]":
                            in_class = False
                        elif ch == "/" and not in_class:
                            closed = True
                            break
                        j += 1
                    if closed:
                        out.append(" ")  # 正则体不参与括号深度
                        i = j + 1
                        continue
            out.append(c)
        elif mode in "\"'":
            if c == "\\":
                i += 2
                continue
            if c == mode:
                mode = None
        elif mode == "/*":
            if line.startswith("*/", i):
                mode = None
                i += 2
                continue
        i += 1
    return "".join(out), mode  # mode 携带到下一行（跨行注释/字符串）

def brackets(cleaned):
    return sum(cleaned.count(c) for c in "([{") - sum(cleaned.count(c) for c in ")]}")

depth = 0
carry_mode = None
d_before, d_after, in_c, cln = [], [], [], []
for idx in range(script_open + 1, script_close):
    line = lines[idx]
    if carry_mode == "/*":
        m = re.search(r"\*/", line)
        if m:
            cleaned, carry_mode = strip_noise(line[m.end():])
            d_before.append(depth)
            in_c.append(False)
            cln.append(cleaned)
            depth += brackets(cleaned)
            d_after.append(depth)
        else:
            d_before.append(depth)
            in_c.append(True)
            cln.append("")
            d_after.append(depth)
        continue
    cleaned, carry_mode = strip_noise(line)
    d_before.append(depth)
    # 行内代码为空且进入未闭合块注释 → 整行是注释
    in_c.append(carry_mode == "/*" and not cleaned.strip())
    cln.append(cleaned)
    depth += brackets(cleaned)
    d_after.append(depth)
    if carry_mode is not None and carry_mode in "\"'":
        carry_mode = None  # 本代码库字符串不跨行

# ---------- 3. 顶层语句切分 ----------
stmts = []  # {start,end}（0-based 行区间，含前置注释）
TOP = script_open + 1  # 第一行脚本内容的下标

def rel(j):
    return j - TOP

i = TOP
prev_stmt_end = TOP - 1  # 上一条语句的结束行（0-based），回溯下界
while i < script_close:
    if in_c[rel(i)] or not cln[rel(i)].strip() or d_before[rel(i)] != 0:
        i += 1
        continue
    # 回收紧邻的前置注释：纯注释行（含块注释中间行）逐行向前收，
    # 遇到代码行 / 空行 / 上一条语句的领地即停（防止区间重叠）。
    k = i
    while k > max(TOP, prev_stmt_end + 1):
        r = rel(k - 1)
        if cln[r].strip():
            break                    # 上一行有真实代码 → 停
        if not lines[k - 1].strip():
            break                    # 上一行是空行 → 停（间距归属下一语句前的空隙）
        # 剩余情况：纯注释行（in_c 或已被剥离干净）→ 收入本语句
        k -= 1
    start = k
    j = i
    end = None
    while j < script_close:
        r = rel(j)
        if in_c[r] or not cln[r].strip():
            j += 1
            continue
        t = cln[r].strip()
        # 语句结束：净深度回到 0 且剥离后行尾是 ; 或 }
        if d_after[r] == 0 and (t.endswith(";") or t.endswith("}")):
            end = j
            break
        j += 1
    if end is None:
        end = script_close - 1
    stmts.append({"start": start, "end": end})
    prev_stmt_end = end
    i = end + 1

# ---------- 4. 语句内的声明名 ----------
def _first_code_line_idx(s):
    """语句内第一行真实代码（跳过前置注释，含跨行块注释全部行）的局部下标。"""
    for k in range(s["end"] - s["start"] + 1):
        r = rel(s["start"] + k)
        if in_c[r] or not cln[r].strip():
            continue
        return k
    return None

def decl_names(s):
    names = []
    base = _first_code_line_idx(s)
    if base is None:
        return names
    code_from = "\n".join(lines[s["start"] + base:s["end"] + 1])
    for ln in lines[s["start"]:s["end"] + 1]:
        m = re.match(r"^function\s+([A-Za-z_$][\w$]*)", ln)
        if m:
            names.append(m.group(1))
    if re.match(r"^var\b", code_from):
        cleaned, _ = strip_noise(code_from)
        bal = 0
        cur = []
        declarators = []
        for ch in cleaned:
            if ch in "([{":
                bal += 1
            elif ch in ")]}":
                bal -= 1
            if ch == "," and bal == 0:
                declarators.append("".join(cur))
                cur = []
            else:
                cur.append(ch)
        declarators.append("".join(cur))
        for d in declarators:
            d = d.strip()
            while d.startswith("var"):
                d = d[3:].strip()
            m = re.match(r"([A-Za-z_$][\w$]*)", d)
            if m:
                names.append(m.group(1))
    return names

for s in stmts:
    s["names"] = decl_names(s)
    s["first_name"] = s["names"][0] if s["names"] else None

# ---------- 5. 目标分片映射 ----------
RANGES = [
    (1013, 1018, "shared/icons"),
    (1019, 1084, "shared/state"),
    (1085, 1085, "shared/dom"),
    (1086, 1089, "shared/time"),
    (1090, 1096, "shared/dom"),
    (1097, 1106, "shared/api-client"),
    (1107, 1111, "shared/dom"),
    (1112, 1119, "shared/time"),
    (1120, 1126, "shared/time"),
    (1128, 1333, "preview/driver"),
    (1334, 1428, "panels/scenes"),
    (1429, 1554, "panels/inspector"),
    (1555, 1770, "timeline/model"),
    (1782, 1977, "timeline/range"),
    (1978, 2323, "timeline/render"),
    (2324, 2415, "panels/status"),
    (2416, 2503, "panels/export"),
    (2504, 2631, "bindings"),
    (2632, 2762, "panels/script"),
    (2763, 2838, "shortcuts"),
    (2839, 2947, "agent/ask"),
    (2948, 3067, "agent/pending"),
    (3068, 3282, "panels/import"),
]
NAME_MAP = {
    "startPhAnim": "preview/playhead",
    "stopPhAnim": "preview/playhead",
    "setPhTransform": "preview/playhead",
    "phFrame": "preview/playhead",
    "fitPreview": "preview/zoom",
    "zoomStep": "preview/zoom",
    "renderInspector": "panels/inspector",
    "applyInspector": "panels/inspector",
    "sceneFields": "panels/scenes",
    "sceneSummary": "panels/scenes",
    "renderSceneList": "panels/scenes",
    "selectScene": "panels/scenes",
    "trackHead": "timeline/render",
    "fitTimeline": "timeline/render",
    "zoomTimeline": "timeline/render",
    "zoomToScene": "timeline/render",
    "snapTime": "timeline/model",
    "renderTimeline": "timeline/render",
    "trimCommit": "timeline/trim",
    "reorder": "timeline/trim",
    "splitSceneData": "timeline/trim",
    "splitScene": "timeline/trim",
    "sameSourceSeam": "timeline/trim",
    "splitAtPlayhead": "timeline/trim",
}

def target_of(s):
    name = s["first_name"]
    if name and name in NAME_MAP:
        return NAME_MAP[name]
    for a, b, f in RANGES:
        if s["start"] + 1 >= a and s["end"] + 1 <= b:
            return f
    for a, b, f in RANGES:
        if s["start"] + 1 >= a - 40 and s["start"] + 1 <= b:
            return f
    raise SystemExit(f"no target for stmt at {s['start']+1}-{s['end']+1}: {lines[s['start']][:60]}")

for s in stmts:
    s["target"] = target_of(s)

mods = {}
for s in stmts:
    mods.setdefault(s["target"], []).append(s)

symbol_owner = {}
for f, ss in mods.items():
    for s in ss:
        for nm in s["names"]:
            if nm in symbol_owner and symbol_owner[nm] != f:
                raise SystemExit(f"dup symbol {nm}: {symbol_owner[nm]} vs {f}")
            symbol_owner[nm] = f

# 跨分片依赖图（M5 ESM/store 化的迁移地图）
graph = {}
for f, ss in mods.items():
    body = "\n".join("\n".join(lines[s["start"]:s["end"] + 1]) for s in ss)
    refs = set()
    for nm, owner in symbol_owner.items():
        if owner != f and re.search(r"\b%s\b" % re.escape(nm), body):
            refs.add(owner)
    graph[f] = sorted(refs)

# 引用环（M5 提示：环上模块需一起 store 化）
func_only = {
    f: all(re.match(r"^function\b", "\n".join(lines[s["start"]:s["end"] + 1])) for s in ss)
    for f, ss in mods.items()
}
index_counter = [0]
stack, lowlink, index, on_stack = [], {}, {}, {}
sccs = []

def strongconnect(v):
    index[v] = lowlink[v] = index_counter[0]
    index_counter[0] += 1
    stack.append(v)
    on_stack[v] = True
    for w in graph[v]:
        if w not in index:
            strongconnect(w)
            lowlink[v] = min(lowlink[v], lowlink[w])
        elif on_stack.get(w):
            lowlink[v] = min(lowlink[v], index[w])
    if lowlink[v] == index[v]:
        comp = []
        while True:
            w = stack.pop()
            on_stack[w] = False
            comp.append(w)
            if w == v:
                break
        sccs.append(comp)

for v in sorted(mods):
    if v not in index:
        strongconnect(v)
cyclic_groups = [sorted(c) for c in sccs if len(c) > 1]

# ---------- 6. 生成 JS 分片（逐字节保留行内容） ----------
order = sorted(mods, key=lambda f: min(s["start"] for s in mods[f]))
first_line = stmts[0]["start"]          # 原脚本第一行（0-based）
last_line = stmts[-1]["end"]            # 原脚本最后一行（0-based）
build_order = []
for f in order:
    ss = sorted(mods[f], key=lambda s: s["start"])
    span_start = ss[0]["start"]
    span_end = ss[-1]["end"]
    # 分片内语句之间的空行原样保留：按绝对行号重拼
    seg_lines = []
    prev_end = None
    for s in ss:
        if prev_end is not None:
            seg_lines.extend([""] * (s["start"] - prev_end - 1))
        seg_lines.extend(lines[s["start"]:s["end"] + 1])
        prev_end = s["end"]
    path = os.path.join(OUT, f + ".js")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(seg_lines) + "\n")
    build_order.append({
        "file": "%s.js" % f,
        "span": [span_start + 1, span_end + 1],          # 1-based 闭区间（可能与其他分片交错）
        "ranges": [[s["start"] + 1, s["end"] + 1] for s in ss],  # 每条语句区间——拼接的唯一事实
        "stmts": len(ss),
        "symbols": sorted({nm for s in ss for nm in s["names"]}),
    })
tail_blanks = script_close - last_line - 1  # 脚本尾与 </script> 之间的空行数

# 隐式全局（副作用语句里赋值但未 var 声明、非形参的名字）——仅供 eslint/M5 参考
declared = set(symbol_owner)
implicit = set()

def _stmt_declared_and_params(s):
    """语句内部 var 声明 + 函数形参名（作用域内已定义，不算隐式全局）。"""
    local = set()
    body = "\n".join(lines[s["start"]:s["end"] + 1])
    for m in re.finditer(r"\bvar\b", body):
        # 从 var 起向前解析顶层逗号分隔的 declarator
        j = m.end()
        bal = 0
        cur = []
        seg = []
        while j < len(body):
            ch = body[j]
            if ch in "([{":
                bal += 1
            elif ch in ")]}":
                bal -= 1
            if ch == ";" and bal == 0:
                break
            if ch == "," and bal == 0:
                seg.append("".join(cur))
                cur = []
            else:
                cur.append(ch)
            j += 1
        seg.append("".join(cur))
        for d in seg:
            d = d.strip()
            m2 = re.match(r"([A-Za-z_$][\w$]*)", d)
            if m2:
                local.add(m2.group(1))
    for m in re.finditer(r"\bfunction\b[^(]*\(([^)]*)\)", body):
        for p in m.group(1).split(","):
            p = p.strip()
            m2 = re.match(r"([A-Za-z_$][\w$]*)", p)
            if m2:
                local.add(m2.group(1))
    return local

RESERVED = {"var", "function", "return", "true", "false", "null", "new",
            "typeof", "this", "window", "document", "undefined", "let",
            "const", "if", "else", "for", "while", "break", "continue",
            "switch", "case", "throw", "try", "catch", "finally", "in",
            "of", "do", "void", "delete", "instanceof", "arguments"}
for s in stmts:
    body = "\n".join(lines[s["start"]:s["end"] + 1])
    body, _ = strip_noise(body)  # 剥离字符串/注释/正则，避免 SVG 属性等误报
    local = _stmt_declared_and_params(s)
    for m in re.finditer(r"(?<![\w$.])([a-zA-Z_$][\w$]*)\s*=(?!=)", body):
        nm = m.group(1)
        if nm not in declared and nm not in local and nm not in RESERVED:
            implicit.add(nm)

# ---------- 7. CSS 拆分 ----------
CSS_SECTIONS = [
    (style_open + 2, 12, "styles/tokens"),
    (13, 34, "styles/base"),
    (35, 59, "styles/topbar"),
    (60, 109, "styles/workspace"),
    (110, 163, "styles/preview"),
    (164, 196, "styles/transport"),
    (197, 220, "styles/inspector"),
    (221, 412, "styles/timeline"),
    (413, 503, "styles/ask"),
    (504, style_close, "styles/import"),   # style_close(0-based) 恰为 1-based 内容末行
]
css_files = []
for a, b, f in CSS_SECTIONS:
    txt = "\n".join(lines[a - 1:b])
    p = os.path.join(OUT, f + ".css")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(txt + "\n")
    css_files.append({"file": "%s.css" % f, "span": [a, b]})

# ---------- 8. index.html 模板 ----------
head_part = "\n".join(lines[0:style_open])                       # 到 <style> 前
mid_part = "\n".join(lines[style_close + 1:script_open])         # </head>…<body>…</head> 后到 <script> 前
script_open_line = lines[script_open]                            # 保留原始缩进（" <script>"）
tail_part = "\n".join(lines[script_close + 1:])                  # </script> 后（</body></html>）
script_close_line = lines[script_close]                     # "</script>"
idx = (head_part + "\n<!--BUILD:CSS-->\n" + mid_part + "\n" + script_open_line
       + "\n<!--BUILD:JS-->\n" + script_close_line + "\n" + tail_part)
os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "index.html"), "w", encoding="utf-8") as fh:
    fh.write(idx)

# ---------- 9. 构建清单 / 依赖图 / 全局符号 ----------
os.makedirs(SCRIPTS, exist_ok=True)
meta = {
    "generated_from": "tools/studio_ui.html",
    "generator": "scripts/migrate-from-monolith.py",
    "css": css_files,
    "js": build_order,
    "head_part_lines": [1, style_open + 1],              # 1-based
    "css_span": [style_open + 2, style_close],           # 1-based CSS 内容行
    "mid_part_lines": [style_close + 2, script_open + 1],
    "script_open_line": script_open_line,
    "js_span": [script_open + 2, script_close],          # 1-based JS 内容行
    "tail_part_lines": [script_close + 2, n],
    "tail_blanks": tail_blanks,
}
with open(os.path.join(SCRIPTS, "build-order.json"), "w", encoding="utf-8") as fh:
    json.dump(meta, fh, ensure_ascii=False, indent=1)
with open(os.path.join(SCRIPTS, "fragment-deps.json"), "w", encoding="utf-8") as fh:
    json.dump({
        "note": "分片间全局符号依赖（M5 store/ESM 化的迁移地图）；环上分片需一起迁移。",
        "deps": graph,
        "cyclic_groups": cyclic_groups,
        "func_only": {k: v for k, v in func_only.items()},
        "symbol_owner": {k: v for k, v in sorted(symbol_owner.items())},
    }, fh, ensure_ascii=False, indent=1)
with open(os.path.join(SCRIPTS, "globals.json"), "w", encoding="utf-8") as fh:
    json.dump({"declared": sorted(declared), "implicit": sorted(implicit)},
              fh, ensure_ascii=False, indent=1)

# ---------- 10. 报告 ----------
report = {
    "stmt_count": len(stmts),
    "fragments": {f: len(mods[f]) for f in order},
    "cyclic_groups": cyclic_groups,
    "symbol_total": len(declared),
    "implicit_globals": sorted(implicit),
    "css_files": [c["file"] for c in css_files],
}
print(json.dumps(report, ensure_ascii=False, indent=1)[:5000])
