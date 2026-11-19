# Floor（launch skill 的底线——gate 检查的每一项）

只列 gate 能检查或将要检查的。gate 还看不见的规则标注"gate 未见"。

| 规则 | 检查 |
| --- | --- |
| hf_validate 0 errors | `python3 tools/hf_validate.py contract.html` exit 0 |
| schema_version = "1.0" | spec.json |
| scenes 非空且 ≤ 上限 | spec.json |
| storyboard.arc 与 frames 非空、与 scenes 同长 | spec.json storyboard 段 |
| 无占位残留（`[slot]` / "占位" / "待填"） | 契约页文本 |
| 每场景有 text 且非空 | spec.json |
| 每场景 duration 0.5–60s | spec.json |
| 音轨存在（narration/BGM）或显式静音声明 | spec.json + contract 页 |
| 画布尺寸与 composition.format 匹配 | spec.json + 契约页 |
| 无 `[slot]` 样式的未填模板 slot | spec.json |

gate 未见（人工/agent look 责任）：文字可读性、hook 是否钩人、
两色纪律是否被破坏、溢出/截断。
