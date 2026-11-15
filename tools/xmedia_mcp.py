#!/usr/bin/env python3
# xmedia-kit MCP server（stdio JSON-RPC 2.0）：把 Studio 的编辑 / 预览 /
# 渲染能力和 ASR / 素材导入暴露给 Claude / Cursor / Codex / WorkBuddy 等
# 支持 MCP 的 Agent，对应 Remotion 演示里的「Full access」。
#
# 启动（由 MCP 客户端托管，不要手动前台跑）：
#   python3 tools/xmedia_mcp.py --studio-url http://127.0.0.1:8120
#
# 配置片段见 docs/HF_COMPOSE.md「MCP server」章节。stdout 严格只输出
# JSON-RPC 行，所有日志走 stderr（MCP 客户端对 stdout 格式敏感）。
#
# 依赖：仅 Python 标准库；state/recompose/tts/bake/agent 需要 8120 Studio
# 服务在跑，import/asr/compose/validate 可直接本地调用（无服务也能用）。

import argparse
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.dirname(TOOLS_DIR)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

PROTOCOL = "2024-11-05"


def log(msg):
    print(f"xmedia_mcp: {msg}", file=sys.stderr, flush=True)


def http_json(studio_url, path, method="GET", payload=None, timeout=300):
    url = studio_url.rstrip("/") + path
    data = None
    headers = {"Accept": "application/json"}
    if payload is not None:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers,
                                 method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:400]
        raise RuntimeError(f"HTTP {e.code} {path}: {body}")
    except urllib.error.URLError as e:
        raise RuntimeError(
            f"无法连接 Studio 服务 {studio_url}（先启动 `xmedia studio ... "
            f"--serve --spec ...`）：{e}")


def spec_of(spec, studio_url):
    return spec if spec is not None else http_json(
        studio_url, "/api/state").get("spec")


TOOL_DEFS = [
    {"name": "xmedia_state",
     "description": "读取当前 Studio 的 spec、场景、时长与契约校验结果",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "xmedia_recompose",
     "description": "用完整 spec JSON 重组合成，Studio 预览立即热更新",
     "inputSchema": {"type": "object", "properties": {
         "spec": {"type": "object", "description": "完整 spec JSON"}},
         "required": ["spec"]}},
    {"name": "xmedia_tts",
     "description": "对 spec 全场景生成 TTS 旁白并回写 narration",
     "inputSchema": {"type": "object", "properties": {
         "spec": {"type": "object"}}}},
    {"name": "xmedia_bake",
     "description": "导出 MP4（底层 html_bake 离线渲染），返回 job id",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "xmedia_job",
     "description": "查询导出任务进度与产物路径",
     "inputSchema": {"type": "object", "properties": {
         "id": {"type": "integer"}}, "required": ["id"]}},
    {"name": "xmedia_agent",
     "description": "用自然语言指令改 spec（本地规则 + 可选 LLM），自动重组",
     "inputSchema": {"type": "object", "properties": {
         "message": {"type": "string"}}, "required": ["message"]}},
    {"name": "xmedia_import",
     "description": "扫描目录里的实拍视频，按实长排时间轴生成 spec（可带 ASR 字幕与片尾卡）",
     "inputSchema": {"type": "object", "properties": {
         "dir": {"type": "string"}, "out": {"type": "string"},
         "asr": {"type": "boolean"}, "captions": {"type": "string"},
         "endcard": {"type": "boolean"}, "format": {"type": "string"}},
         "required": ["dir"]}},
    {"name": "xmedia_asr",
     "description": "把音视频语音或 SRT/VTT 转成带时间戳的 caption segments",
     "inputSchema": {"type": "object", "properties": {
         "media": {"type": "string"}, "provider": {"type": "string"},
         "srt": {"type": "string"}}, "required": ["media"]}},
    {"name": "xmedia_compose",
     "description": "本地把 spec 编译成契约 HTML 并校验（不需要 Studio 服务）",
     "inputSchema": {"type": "object", "properties": {
         "spec": {"type": "object"}, "out": {"type": "string"}},
         "required": ["spec"]}},
    {"name": "xmedia_validate",
     "description": "校验 spec 能否通过 hf_compose + hf_validate（Agent 写 spec 的硬门）",
     "inputSchema": {"type": "object", "properties": {
         "spec": {"type": "object"}}, "required": ["spec"]}},
    {"name": "xmedia_help",
     "description": "返回 spec schema 摘要与常用工作流",
     "inputSchema": {"type": "object", "properties": {}}},
]


def call_tool(name, args, studio_url):
    if name == "xmedia_state":
        return http_json(studio_url, "/api/state")
    if name == "xmedia_recompose":
        return http_json(studio_url, "/api/recompose", "POST",
                         {"spec": args["spec"]})
    if name == "xmedia_tts":
        return http_json(studio_url, "/api/tts", "POST",
                         {"spec": spec_of(args.get("spec"), studio_url)})
    if name == "xmedia_bake":
        return http_json(studio_url, "/api/bake", "POST", {})
    if name == "xmedia_job":
        return http_json(studio_url, "/api/job?id=" + str(int(args["id"])))
    if name == "xmedia_agent":
        return http_json(studio_url, "/api/agent", "POST",
                         {"message": args["message"]})
    if name == "xmedia_import":
        import hf_import
        out = os.path.abspath(os.path.expanduser(args["out"])) if args.get("out") else ""
        spec_dir = os.path.dirname(out) if out else os.path.expanduser(args["dir"])
        spec, notes = hf_import.build_spec(
            os.path.expanduser(args["dir"]), spec_dir,
            fmt=args.get("format") or "16:9", asr=bool(args.get("asr")),
            captions_file=args.get("captions") or "",
            endcard=bool(args.get("endcard")))
        result = {"spec": spec, "notes": notes}
        if out:
            os.makedirs(spec_dir, exist_ok=True)
            with open(out, "w", encoding="utf-8") as f:
                json.dump(spec, f, ensure_ascii=False, indent=2)
            result["out"] = out
        return result
    if name == "xmedia_asr":
        import hf_asr
        return hf_asr.transcribe(
            os.path.expanduser(args["media"]),
            provider=args.get("provider") or "auto",
            srt_path=args.get("srt") or None)
    if name == "xmedia_compose":
        import hf_compose
        import hf_validate
        spec = args["spec"]
        raw_out = args.get("out") or ""
        out = os.path.abspath(os.path.expanduser(raw_out)) if raw_out else ""
        hf_compose._spec_dir = os.path.dirname(out) or os.getcwd()
        hf_compose._gsap_src = hf_compose._load_gsap()
        html = hf_compose.compose(spec)
        findings, errors, warnings, infos = hf_validate.validate(html)
        result = {"ok": errors == 0, "errors": errors, "bytes": len(html),
                  "findings": [{"level": lv, "message": msg}
                               for lv, msg in findings]}
        if out:
            with open(out, "w", encoding="utf-8") as f:
                f.write(html)
            result["out"] = out
        else:
            result["html_head"] = html[:400]
        return result
    if name == "xmedia_validate":
        import hf_compose
        import hf_validate
        import hf_agent
        spec = args["spec"]
        hf_agent.validate_spec(spec)
        with tempfile.TemporaryDirectory(prefix="xmedia_mcp_") as tmp:
            hf_compose._spec_dir = tmp
            hf_compose._gsap_src = hf_compose._load_gsap()
            html = hf_compose.compose(spec)
            findings, errors, warnings, infos = hf_validate.validate(html)
            return {"ok": errors == 0, "errors": errors,
                    "findings": [{"level": lv, "message": msg}
                                 for lv, msg in findings]}
    if name == "xmedia_help":
        return {
            "pipeline": "spec JSON → hf_tts → hf_compose → hf_validate → "
                        "Studio 预览 → html_bake → MP4",
            "spec": {
                "schema_version": "1.0",
                "composition": "{id, format(16:9/9:16/1:1/4:5), fps}",
                "scenes[]": "{type, text, duration, bg, animation, video, "
                            "narration, captions[]}",
                "captions": "{enabled, segments:[{start,end,text}]}（全局绝对秒）",
                "audio": "{src, volume, fade_in, fade_out}",
                "sfx": "{enabled}", "transitions": "{type, duration}"},
            "scene_types": ["title", "text", "outro", "quote", "list",
                            "stats", "comparison", "自定义模板名"],
            "docs": "docs/HF_COMPOSE.md",
            "skill": ".agents/skills/xmedia-ai-video/SKILL.md",
        }
    raise RuntimeError(f"未知 tool：{name}")


def handle_request(req, studio_url):
    method = req.get("method")
    rid = req.get("id")
    params = req.get("params") or {}
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": PROTOCOL, "capabilities": {"tools": {}},
            "serverInfo": {"name": "xmedia-kit", "version": "1.0"}}}
    if method == "ping":
        return {"jsonrpc": "2.0", "id": rid, "result": {}}
    if method in ("notifications/initialized", "notifications/cancelled"):
        return None
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": rid,
                "result": {"tools": TOOL_DEFS}}
    if method == "tools/call":
        name = params.get("name", "")
        args = params.get("arguments") or {}
        try:
            result = call_tool(name, args, studio_url)
            text = json.dumps(result, ensure_ascii=False, indent=2)
            if len(text) > 30000:
                text = text[:30000] + "\n…（结果已截断）"
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": text}],
                "isError": False}}
        except Exception as e:
            return {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": f"错误：{e}"}],
                "isError": True}}
    if method in ("resources/list", "prompts/list"):
        key = "resources" if method.startswith("resources") else "prompts"
        return {"jsonrpc": "2.0", "id": rid, "result": {key: []}}
    return {"jsonrpc": "2.0", "id": rid,
            "error": {"code": -32601, "message": f"Method not found: {method}"}}


def main():
    p = argparse.ArgumentParser(description="xmedia-kit MCP server (stdio)")
    p.add_argument("--studio-url", default=os.environ.get(
        "XMEDIA_STUDIO_URL", "http://127.0.0.1:8120"))
    args = p.parse_args()
    log(f"就绪，Studio={args.studio_url}，tools={len(TOOL_DEFS)}")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            log("忽略非 JSON 输入行")
            continue
        try:
            resp = handle_request(req, args.studio_url)
        except Exception as e:
            resp = {"jsonrpc": "2.0", "id": req.get("id"),
                    "error": {"code": -32603, "message": str(e)}}
        if resp is not None:
            print(json.dumps(resp, ensure_ascii=False), flush=True)


if __name__ == "__main__":
    main()
