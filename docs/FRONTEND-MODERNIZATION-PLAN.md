# Studio 前端现代化计划（M1–M7）

> 状态：M1–M4、M7 完成；M5/M6 核心完成（剩余项见 §6 里程碑表与
> [FRONTEND-MODERNIZATION.md](FRONTEND-MODERNIZATION.md) §7.6）。
>
> **版本说明（重要）**：本文档原版在 2026-11-17 的回合清理中随未跟踪
> 新文件一并丢失，当前为**同日重建版**。重建保留了全部 operative 内容
> （里程碑状态、两处决策偏差、四条硬性不变量、验收标准、延后项与
> 触发条件）；原版含更细的逐阶段实施细节（逐文件行号映射等），其中
> 已执行部分以执行记录与决策记录为准——
> [FRONTEND-MODERNIZATION.md](FRONTEND-MODERNIZATION.md)。

## 0. 为什么做

- `tools/studio_ui.html` 已达 3285 行（CSS 621 + JS 约 2270 + HTML 460），
  16 个功能域、约 120 个顶层函数全部内联，改动回归风险随体积超线性增长。
- 前端此前零测试、零类型、零 lint；同仓库 C++ 侧已有完备门禁
  （`gate_local.sh`），质量标准不对称。
- /api 契约（14 个端点）只存在于 `hf_studio.py` 的分支里，前后端无共享
  schema，无自动护栏。
- 项目锁定 CEF 156（Chromium 2026 年代版本），平台新特性基本全量可用。

## 1. 参考基准与两条推进轴

参考 [GoogleChrome/modern-web-guidance](https://github.com/GoogleChrome/modern-web-guidance)
（Chrome/Edge/Firefox/MDN 维护的现代 Web 平台特性编码规范，附离线 CLI）。
其定位是"平台特性使用规范"而非"架构风格指南"，因此按两条轴推进：

- **工程生态轴**：构建 / 类型 / lint / 测试 / 契约 / CI——补齐生态完整性；
- **平台现代性轴**：对齐 mwg 学科清单（UI Atoms、a11y、性能、安全）。

## 2. 目标架构与关键决策

### 2.1 源码与产物

- 源码：`tools/web/src/`（styles 每面板一文件 + JS 域分片 + schemas +
  index.html 模板）；**产物路径不变**（`tools/studio_ui.html`，npm `files`
  与 `hf_studio.py` 磁盘读取均不感知构建）。
- 产物形态：单文件自包含（JS/CSS 内联、离线可用）——npm 分发与
  CEF/系统浏览器双运行时的硬约束。

### 2.2 技术选型（生态完整性优先，全部为事实标准）

Vite + vite-plugin-singlefile ｜ TypeScript（渐进 strict）｜ ESLint flat
config + Prettier + Stylelint ｜ zod（/api 边界）｜ JSON Schema 单一事实源
（Python/TS 共享）｜ Vitest ｜ Playwright ｜ axe-core。

### 2.3 组件化选型：Web Components（Custom Elements），ADR 触发条件

产物约束（单文件、离线、CEF+系统浏览器）使框架运行时收益打折；
mwg 立场是"优先原生平台能力"。引入框架的触发条件（写进 ADR）：
手写 store 出现明显 bug 率，或团队扩到 3+ 人高频并行改 UI。

### 2.4 分阶段原则

1. 每阶段独立可合入、产物路径始终不变；
2. 先护栏后动刀（Phase 0 黄金基线先行）；
3. 行为迁移优先于美化（M3"行为零变化"，M4/M5 才允许新特性）。

## 3. 硬性不变量（Do-Not-Regress 清单）

| # | 不变量 | 状态 |
|---|---|---|
| I-1 | **合成层播放头**：播放中绝不写 transform，位置由 CSS transition 在合成层插值；JS rAF 只做低频文本回填 | ✓ e2e + 托管浏览器断言 |
| I-2 | **脏检查 DOM 写入**：高频 UI 先比对上次内容再写 | ✓ 逻辑逐字节保留 |
| I-3 | **预览协议兼容链**：iframe 优先 `__mcStudioAPI` 直调，退化 postMessage；兼容旧服务进程 | ✓ 保留（协议版本化待后续） |
| I-4 | **音频自动播放解锁**：首次用户手势内调 enableAudio，失败重试 | ✓ 保留 |

全局不变量：产物离线自包含（`/api/*` 之外无网络请求）。

## 4. 分阶段路线图（九阶段）

- **Phase 0 行为护栏**：Playwright 黄金基线（截图 + /api/state fixture +
  交互脚本）；
- **Phase 1 工具链地基**：tools/web 骨架、TS/ESLint/Prettier/Stylelint、
  mwg CLI 接入、CI web job + gate_local web job、freshness 检查；
- **Phase 2 源码拆分**：3285 行单文件 → 模块化分片，行为零变化；
- **Phase 3 类型与契约**：JSON Schema 单一事实源、zod 边界校验、
  契约测试、AbortController 与协议版本化；
- **Phase 4 状态与组件化**：store、Web Components、平台 UI Atoms；
- **Phase 5 平台现代性专项**：mwg 特性逐条登记（特性→落点→支持面→降级）；
- **Phase 6 可访问性与安全**：aria/焦点/对比度、postMessage/CSP/供应链；
- **Phase 7 质量门禁收口**：覆盖率门槛、视觉回归、性能预算；
- **Phase 8 文档与流程**：ADR、CONTRIBUTING、协议文档。

## 5. 迁移风险登记（关键四条）

| 风险 | 缓解 |
|---|---|
| npm 用户混跑新旧 UI/服务 | 预览协议兼容链保留（I-3）+ 协议版本化 |
| 拆分引入行为漂移 | 纯函数先抽后测；分片逐字节切片；黄金基线回归 |
| 产物覆盖与发布链冲突 | 产物路径不变 + freshness 门禁语义对齐 C++ 侧机制 |
| 契约双源漂移 | fixtures 对拍测试强制两端同批输入 |

## 6. 里程碑与状态

| 里程碑 | 阶段 | 工作量 | 交付标志 | 状态 |
|---|---|---|---|---|
| M1 安全网 | Phase 0 | 1–2 天 | 黄金基线测试入库并可拦回归 | ✅ 2026-11-17 |
| M2 工具链 | Phase 1 | 2–3 天 | `gate_local.sh --jobs web` 全绿 | ✅ 2026-11-17 |
| M3 模块化 | Phase 2 | 3–5 天 | 目录结构落地，行为零变化 | ✅ 2026-11-17（构建形态偏差见 FRONTEND-MODERNIZATION.md §2.1） |
| M4 契约 | Phase 3 | 3–5 天 | schema 单一事实源 + 双端契约测试 | ✅ 2026-11-17（spec/state schema + zod 契约测试；AbortController/协议版本化顺延） |
| M5 组件化 | Phase 4 | 5–8 天 | `xms-*` 组件 + store，I-1/I-2 无回退 | 🔶 大部分完成（store 桥+订阅、imp/exp 原生 dialog、ESM+Vite——见 FRONTEND-MODERNIZATION.md §6/§7；timeline/inspector/transport 等深度纠缠组件按 ADR 延后） |
| M6 现代性 | Phase 5+6 | 5–9 天 | 平台特性 + a11y/安全收口 | 🔶 核心完成（structuredClone、prefers-reduced-motion、aria×39、postMessage 收紧；`:has()`/View Transitions 等 additive 项待 UI 稳定后逐项落地） |
| M7 收口 | Phase 7+8 | 3–5 天 | 门禁/文档/ADR 齐备 | ✅ 完成（e2e 入 CI web job；文档/ADR/决策记录齐备） |

总量约 5–7 周（单人、含测试）；实际以两轮 agent 会话完成 M1–M4 全部与
M5/M6/M7 的核心交付。

## 7. 附录：modern-web-guidance 工作流对接

```bash
npx modern-web-guidance@latest install                 # 一次性安装
npx modern-web-guidance search "animate a dialog modal backdrop"
npx modern-web-guidance retrieve "animate-to-from-top-layer"
npx modern-web-guidance legacy-scan src                # 存量 legacy 扫描
```

- `.mwgrc` 默认保守：`allowOriginTrials: false`、`allowVolatileFeatures: false`；
- `npm run mwg:scan` 已接入 tools/web；
- AI 辅助开发的约束：涉及 DOM/CSS/弹层的改动，先 search 对应用例、
  按 guide 落地，再让 legacy-scan 与 ESLint 兜底。

## 8. 执行记录（2026-11-17，M1–M5/M6/M7）

M1–M4 全部与 M5/M6/M7 核心已落地，落地时的决策偏差与逐条验收证据
集中在 [FRONTEND-MODERNIZATION.md](FRONTEND-MODERNIZATION.md)。要点：

- **构建形态**：M3 用「逐字节分片 + 确定性拼接构建」替代原计划的
  Vite/ESM——ESM 导入绑定不可跨模块赋值，机械转换必然改行为；
  产物与迁移基线**逐字节一致**。M5 store 桥就位后切换 Vite，
  产物 minify -33%。
- **store**：32 个跨分片写（rebind）标量收进单一对象 S + globalThis
  访问器桥——分片业务代码零改写；两个引用环的 var 交叉写全部解环。
- **门禁接线**：`gate_local.sh` 新增 `web` job（进 DEFAULT 集合）；
  CI `web` job = hermetic gate + 真实链路 e2e（hf_studio --serve +
  gallery 契约 + Playwright Chromium）。
- **契约**：`src/schemas/{spec,state}.schema.json` 从服务端代码提取
  （期间发现并记录 `agent.cli` 实为 `string|null`）；zod 契约测试与
  黄金 fixture 对拍。AbortController 化与协议版本化待后续（与
  store 演进同 PR 更内聚）。
- **迁移期捕获的三类真实缺陷**：`phTick++` 复合写漏判、`$` 非单词
  字符导致 16 分片漏 import、`state` 重绑定——均已修复并留档
  （FRONTEND-MODERNIZATION.md §6.3）。
