#!/usr/bin/env python3
"""M5 一次性 codemod：全局分片 → ESM + 桥式 store。

机制（为什么零行为变化）：
  * 跨分片写（rebind）的可变标量收进 shared/store.js 的单一对象 S，并以
    访问器桥回 globalThis——分片代码保持原名读写（严格模式下经 setter
    合法），业务代码零改写；
  * 其余符号（函数/常量）逐分片补 import/export；纯函数环
    （driver↔playhead、panels↔timeline）由 ESM 函数提升天然支持；
  * main.js 的 import 顺序 = 原行序（store.js 最先，bindings 最后），
    顶层副作用执行顺序不变。
同时：splitSceneData（纯数据）trim→model、scriptTokenize（纯文本）
script→shared/tokenize.js，让单测走直接 import（不触碰 DOM 分片）。
构建切换：Vite + vite-plugin-singlefile 接管（build.mjs 退役）。
"""

import json
import os
import re

WEB = "/home/vanta/work/dsh-wk/xmedia-kit/tools/web"
SRC = os.path.join(WEB, "src")
meta = json.load(open(os.path.join(WEB, "scripts", "build-order.json")))
deps = json.load(open(os.path.join(WEB, "scripts", "fragment-deps.json")))
owner = {k: v + ".js" for k, v in deps["symbol_owner"].items()}

frag_files = {f["file"]: f["ranges"] for f in meta["js"]}
raw = {f: open(os.path.join(SRC, f)).read() for f in frag_files}
lines = {f: (t[:-1] if t.endswith("\n") else t).split("\n") for f, t in raw.items()}

def clean(code):
    code = re.sub(r"//[^\n]*", "", code)
    code = re.sub(r"/\*.*?\*/", "", code, flags=re.S)
    code = re.sub(r"'(?:\\.|[^'\\])*'", "''", code)
    code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
    return code

cleaned = {f: clean("\n".join(lines[f])) for f in frag_files}

# ---------- 1b. 每分片文件的逐行深度表（与迁移脚本同款机制） ----------
def strip_full(line):
    line = re.sub(r"//.*$", "", line)
    line = re.sub(r"'(?:\\.|[^'\\])*'", "''", line)
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    line = re.sub(r"/\*.*?\*/", " ", line)
    return line

def depth_tables(f):
    ls = lines[f]
    depth = 0
    d_before, d_after, cln = [], [], []
    for ln in ls:
        d_before.append(depth)
        c = strip_full(ln)
        for ch in c:
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
        d_after.append(depth)
        cln.append(c)
    return d_before, d_after, cln

DEPT = {f: depth_tables(f) for f in frag_files}

# ---------- 1. store 成员（跨分片写，排除 var 声明与 owner 自身） ----------
assign, vardecl = {}, set()
ASSIGN_PAT = re.compile(r"(?<![\w$.])([A-Za-z_$][\w$]*)\s*(?:=(?!=)|\+\+|--|\+=|-=|\*=|/=|%=)")
for f in frag_files:
    for m in re.finditer(r"\bvar\s+([A-Za-z_$][\w$]*)", cleaned[f]):
        vardecl.add((m.group(1), f))
    for m in ASSIGN_PAT.finditer(cleaned[f]):
        assign.setdefault(m.group(1), set()).add(f)

STORE = []
for nm, own in sorted(owner.items()):
    cross = {s for s in assign.get(nm, set()) if s != own}
    real = {s for s in cross if (nm, s) not in vardecl}
    if real:
        STORE.append((nm, own, sorted(real)))
STORE_SET = {nm for nm, _, _ in STORE}
assert "state" in STORE_SET, "state 必须入 store（refresh() 有整体重绑定）"

# ---------- 2. 分片文件行上的语句查找（深度表驱动） ----------
def find_stmt(f, start_pat, contain=None):
    """在分片文件里找顶层语句：返回 (首行, 末行, 文本)（0-based 行号）。"""
    d_before, d_after, cln = DEPT[f]
    ls = lines[f]
    i = 0
    while i < len(ls):
        if d_before[i] == 0 and re.match(start_pat, ls[i]):
            j = i
            while j < len(ls):
                t = cln[j].rstrip()
                if d_after[j] == 0 and (t.endswith(";") or t.endswith("}")):
                    break
                j += 1
            full = "\n".join(ls[i:j + 1])
            if contain is None or re.search(r"\b%s\b" % re.escape(contain), full):
                return (i, j, full)
            i = j + 1
        else:
            i += 1
    return None

def stmt_comment_start(f, first_line):
    ls = lines[f]
    cs = first_line
    while cs > 0:
        p = ls[cs - 1].strip()
        if not p or p.startswith("/*") or p.startswith("*") or p.startswith("//") or p.endswith("*/"):
            cs -= 1
        else:
            break
    return cs

def split_declarators(stmt_text):
    t = re.sub(r"^\s*var\s+", "", stmt_text)
    bal, cur, parts = 0, [], []
    for ch in t:
        if ch in "([{":
            bal += 1
        elif ch in ")]}":
            bal -= 1
        if ch == "," and bal == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(ch)
    parts.append("".join(cur))
    out = []
    for p in parts:
        p = p.strip()
        m = re.match(r"([A-Za-z_$][\w$]*)\s*=(.*)$", p, re.S)
        if m:
            out.append((m.group(1), p))
    return out

# ---------- 3. 摘出 store 成员声明 ----------
store_init = []
delete_rows = {f: set() for f in frag_files}
rewrite_var = {}

for nm, own, _ in STORE:
    hit = find_stmt(own, r"var\b", contain=nm)
    assert hit, f"store 成员 {nm} 在 {own} 未找到 var 声明"
    fs, fe, text = hit
    cs = stmt_comment_start(own, fs)
    comment = "\n".join(lines[own][cs:fs])
    decls = split_declarators(text)
    keep = [d for d in decls if d[0] not in STORE_SET]
    drop = [d for d in decls if d[0] in STORE_SET]
    for d in drop:
        if not any(x[0] == d[0] for x in store_init):
            store_init.append((d[0], d[1], comment))
    if keep:
        rewrite_var.setdefault(own, []).append(
            (text, "var " + ", ".join(d[1] for d in keep) + ";"))
        for ln in range(cs, fs):
            delete_rows[own].add(ln)     # 只删注释行，语句本体走文本重写
    else:
        for ln in range(cs, fe + 1):
            delete_rows[own].add(ln)     # 整语句（含注释）删除

# ---------- 4. MOVE：splitSceneData→model、scriptTokenize→tokenize ----------
MOVES = [
    ("splitSceneData", "timeline/trim.js", "timeline/model.js"),
    ("scriptTokenize", "panels/script.js", "shared/tokenize.js"),
]
moved_text = {}
for fname, srcf, dstf in MOVES:
    hit = find_stmt(srcf, r"function\s+%s\b" % re.escape(fname))
    assert hit, fname
    fs, fe, text = hit
    moved_text.setdefault(dstf, []).append(text)
    for ln in range(fs, fe + 1):
        delete_rows[srcf].add(ln)

# ---------- 5. 重写分片（删行 → var 文本重写 → 追加移动函数） ----------
new_files = {}
for f in frag_files:
    keep_lines = [ln for i, ln in enumerate(lines[f]) if i not in delete_rows[f]]
    joined = "\n".join(keep_lines)
    for old_text, new_text in rewrite_var.get(f, []):
        keep_first = [d for d in split_declarators(old_text) if d[0] not in STORE_SET]
        m = re.search(r"var\s+" + re.escape(keep_first[0][1][:40]), joined)
        assert m, (f, keep_first[0][0])
        end = joined.find(";", m.start())
        stmt_old = joined[m.start():end + 1]
        joined = joined[:m.start()] + new_text + joined[end + 1:]
    ls = joined.split("\n")
    for text in moved_text.get(f, []):
        ls += ["", text]
    new_files[f] = ls

# ---------- 6. import/export 接线 ----------
all_symbols = set(owner.keys())
bridged = STORE_SET

def rel_path(from_file, to_file):
    d = os.path.dirname(from_file)
    rel = os.path.relpath(to_file, d) if d else to_file
    return rel if rel.startswith(".") else "./" + rel

wired = {}
for f in frag_files:
    code = "\n".join(new_files[f])
    cl = clean(code)
    own_here = {nm for nm, o in owner.items() if o == f}
    used = {}
    for nm in all_symbols:
        if nm in own_here or nm in bridged:
            continue
        if re.search(r"\b%s\b" % re.escape(nm), cl):
            used.setdefault(owner[nm], []).append(nm)
    imports = []
    for srcf in sorted(used):
        names = ", ".join(sorted(used[srcf]))
        imports.append('import { %s } from "%s";' % (names, rel_path(f, srcf)))
    out = []
    for ln in new_files[f]:
        if re.match(r"^function\s+[A-Za-z_$]", ln):
            ln = "export " + ln
        elif re.match(r"^var\s+[A-Za-z_$]", ln):
            ln = "export " + ln
        out.append(ln)
    header = "\n".join(imports) + ("\n\n" if imports else "")
    wired[f] = header + "\n".join(out) + "\n"

for dstf, texts in moved_text.items():
    if dstf not in wired:
        body = "\n\n".join(texts)
        body = re.sub(r"^function\s+", "export function ", body, count=1)
        wired[dstf] = body + "\n"

# ---------- 7. store.js / main.js / index.html ----------
store_lines = [
    "/* Studio 全局可变状态（M5 store）。",
    " *",
    " * 跨分片写（rebind）的可变标量收进单一对象 S；分片代码经访问器桥保持",
    " * 原名读写（见下方 defineProperty），业务代码零改写。playback 组",
    " * （ph*/playing/lastTime…）按 I-1 不走响应式订阅——位置推进始终由",
    " * 合成层动画承担，rAF 时钟只做低频文本回填。",
    " */",
    "export const S = {",
]
seen = set()
for nm, decl, comment in store_init:
    if nm in seen:
        continue
    seen.add(nm)
    if comment:
        store_lines.append(comment)
    store_lines.append("  " + decl + ",")
store_lines += [
    "};",
    "",
    "// 访问器桥：globalThis 上的同名 accessor。严格模式下 `name = v` 经",
    "// setter 合法（不是未声明变量赋值）。store.js 必须是 main.js 的",
    "// 第一个 import——任何分片的顶层副作用求值前桥必须就位。",
    "for (const k of Object.keys(S)) {",
    "  Object.defineProperty(globalThis, k, {",
    "    get() { return S[k]; },",
    "    set(v) { S[k] = v; },",
    "    configurable: true,",
    "  });",
    "}",
]
wired["shared/store.js"] = "\n".join(store_lines) + "\n"

order = [f["file"] for f in meta["js"]]
main_lines = [
    "// Studio UI 入口（M5 ESM）。import 顺序 = 原行序；store.js 最先，",
    "// 顶层副作用执行顺序与迁移前一致。",
]
for css in ["tokens", "base", "topbar", "workspace", "preview", "transport",
            "inspector", "timeline", "ask", "import"]:
    main_lines.append('import "./styles/%s.css";' % css)
main_lines.append('import "./shared/store.js";')
for f in order:
    main_lines.append('import "./%s";' % f)
wired["main.js"] = "\n".join(main_lines) + "\n"

old_tpl = open(os.path.join(SRC, "index.html")).read()
mid_start = old_tpl.index("<!--BUILD:CSS-->")
mid_end = old_tpl.index("<!--BUILD:JS-->")
head_part = old_tpl[:mid_start].rstrip("\n")
mid_part = old_tpl[old_tpl.index("\n", mid_start) + 1: old_tpl.rindex("\n", 0, mid_end)]
idx = (head_part + "\n" + mid_part.rstrip("\n") +
       '\n<script type="module" src="/main.js"></script>\n</body>\n</html>\n')
wired["index.html"] = idx

# ---------- 8. 写盘 ----------
for f, content in wired.items():
    p = os.path.join(SRC, f)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(content)

bm = os.path.join(WEB, "scripts", "build.mjs")
    # build.mjs 受会话保护不可删除——保留为 M3 历史记录，docs 已注明退役
if False and os.path.exists(bm):
    os.remove(bm)

print("store 成员:", len(seen))
print("接线分片:", len([f for f in wired if f.endswith(".js") and f not in ("main.js", "shared/store.js")]))
print("新文件:", sorted(set(wired) - set(frag_files)))
