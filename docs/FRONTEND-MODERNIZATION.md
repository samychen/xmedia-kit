# Studio 前端现代化（M3/M5）——现状与决策记录

> 状态：M3 完成（2026-11-17）；M5 第一阶段（store 化 + ESM + Vite）与
> M5 后半段 + M6 + M7（2026-11-17）完成。决策全文见
> [FRONTEND-MODERNIZATION-PLAN.md](FRONTEND-MODERNIZATION-PLAN.md)；
> 本文记录**落地时的最终决策**（与原计划的两处偏差）与验收证据。
>
> 版本说明：本文与计划文档在回合清理中一度丢失，均为同日重建版——
> 全部 operative 内容（决策/证据/状态）来自执行过程，逐字可溯。

## 1. M3 交付了什么

`tools/studio_ui.html`（3285 行单文件）拆为 `tools/web/src/` 下的模块化源码：

| 层 | 内容 | 数量 |
|---|---|---|
| `src/styles/*.css` | tokens / base / 每面板一文件 | 10 |
| `src/*.js` 域分片 | shared（icons/state/dom/time/api-client）、preview（driver/playhead/zoom）、timeline（model/range/render/trim）、panels×6、agent×2、bindings、shortcuts | 22 |
| `src/schemas/*.json` | spec / /api/state 的 JSON Schema（从前端消费方与服务端 handler 提取） | 2 |
| `tests/unit/` | vitest：纯函数断言 + /api/state 契约测试 | 2 文件 |
| `tests/e2e/run.mjs` | Playwright 端到端基线回归（10 项交互断言） | 1 |
| `scripts/` | 迁移器（可无损重建）+ 确定性拼接构建 + 新鲜度门禁 | 3 |

## 2. 与原计划的两处偏差（决策）

### 2.1 构建形态：M3 用确定性拼接，M5 切 Vite（原计划 Vite 一步到位）

**原因**：ESM 的导入绑定不可跨模块赋值。原 UI 是全局作用域程序，
`playing = true` 这类跨域重赋值遍布全部 22 个域；机械转 ESM 会在运行时
抛 `Assignment to constant variable`，或者迫使把数百处引用改写成
store 访问——两者都违反 M3 的「行为零变化」铁律。

**落地**：M3 分片 = 原文件的逐字节切片，构建按 `build-order.json` 记录的
语句区间依原行序拼接，产物与迁移基线**逐字节一致**（`build.mjs` 每次构
建后校验）。M5 store 桥就位后（见 §6），全局可变状态消失，ESM 才是零
行为差的——构建随之切换 Vite + vite-plugin-singlefile。

**收益不缩水**：文件域拆分、依赖图（`fragment-deps.json`）、
lint/test/build/新鲜度门禁全部就位——这些才是可维护性的本体。

### 2.2 测试分层：纯函数直连源模块，UI 走 Playwright

纯数据函数（时间码/吸附/切割/字幕词元）在 M5 后**直接 import 源分片**
断言——store 的访问器桥在 node 下同样生效。UI 行为（播放推进/面板开合/
快捷键）由 `tests/e2e/run.mjs` 对真实服务断言，控制台零 error 是硬门槛。
像素级断言不做：M3 时代的逐字节一致性已是更强的证据，M5 后由
e2e + 新鲜度门禁接力。

## 3. M3 验收证据（完成定义逐条）

| 验收项 | 证据 |
|---|---|
| 产物与基线逐字节一致 | `build ok … baseline parity: byte-identical`（构建每次自检） |
| 门禁 | `gate_local.sh --jobs web` → PASS；CI 新增 `web` job（hermetic） |
| 类型 | `tsc --noEmit` 零错误 |
| lint | eslint 0 error（140 warning = 分片全局符号预期形态）；stylelint 0 error |
| 单测 | vitest 23/23 |
| e2e | 10/10 交互断言 + 控制台零 error（对运行中的服务） |
| 契约 | `spec.schema.json` 接受全部 3 个 gallery spec；`state.schema.json` 与运行中 `/api/state` 逐字段核对（期间修正 `agent.cli` 为 `string|null`） |
| 不变量 | `check_invariants.py` 684 文件全过 |

## 4. M5 前的迁移地图（谁和谁耦合）

`scripts/fragment-deps.json` 记录分片间全局符号依赖。两个引用环是
M5 store 化的工作量所在：

- `preview/driver ↔ preview/playhead`（播放状态互写）
- `panels/{inspector,scenes,status} ↔ timeline/{model,range,render,trim}`
  （UI 面板与时间轴渲染互调）

store 化顺序：先把这两个环里的可变标量收进 store，环自然解开，再逐分片
补 `import/export` 转 ESM，最后 Vite 接管构建。→ 已按此执行（§6）。

## 5. 已知边界（M3 时代记录，M5 后部分失效）

- `tools/studio_ui.html` 是产物：手改会被 `check-freshness` 拦截。
- ~~分片文件的 lint 以 0 error 为门槛；`no-undef` 在分片上关闭~~
  → M5 已恢复全量 no-undef（ESM 显式 import；桥接名在 eslint globals）。
- e2e 需要运行中的 studio 服务（`hf_studio.py --serve`）与 Chromium；
  M7 起已纳入 CI `web` job 的 e2e 步骤。

## 6. M5 第一阶段：store 化 + ESM + Vite（2026-11-17）

### 6.1 store：单一对象 + 访问器桥，业务代码零改写

全量符号分析（含 `++`/`+=` 复合写）锁定 **32 个跨分片写（rebind）的可变
标量**（含 `state` 本身——`refresh()` 有 `state = s` 整体重绑定）。它们收进
`src/shared/store.js` 的单一对象 `S`，并以 `Object.defineProperty(globalThis)`
访问器桥回原名——分片业务代码**一行业务逻辑都不用改写**（严格模式下
`name = v` 经 setter 合法）。相比"改写数百处 `S.` 前缀"，这条路把行为
风险降到零，同时保住 store 的全部工程收益：单一状态容器、
accessor 可观测、环上标量不再进 import 图。

- 两个引用环中的 var 交叉写全部经桥解环；剩余环均为纯函数环，
  由 ESM 函数提升天然支持（ESM 规范允许函数声明环）。
- **I-1 不变量**：playback 组不走响应式订阅，位置推进仍由合成层动画
  承担（e2e + 托管浏览器断言 `translate3d` transform ✓）。
- **I-2 不变量**：脏检查 DOM 写入逻辑逐字节保留 ✓。

### 6.2 ESM + Vite

- 22 个分片全部转为 ESM（显式 import/export）；`main.js` 的 import 顺序
  = 原行序，顶层副作用执行顺序不变。
- 构建切换为 Vite + vite-plugin-singlefile（`vite.config.ts` Phase 1 预置
  届时生效）；产物 99.5KB（原 147.8KB，minify 收益 33%）。
- eslint no-undef **全量启用**（分片间依赖必须显式 import；32 个桥接名
  在 eslint globals 声明为 writable）——这是 ESM 化后的正确性网。
- 新鲜度门禁语义不变：`dist/index.html` 必须与提交的
  `tools/studio_ui.html` 逐字节一致。

### 6.3 迁移期捕获的三类真实缺陷（codemod 自身也错了三次）

1. `phTick++` 等复合写不在 `name =` 扫描内 → 扩展 ASSIGN 模式后才完整
   （`++` 漏判会让 playhead.js 的低频回填计数脱管）。
2. **`$` 的 import 全量缺失**：符号扫描用 `\b$\b`——`$` 是非单词字符，
   正则边界永不匹配，16 个分片漏 import（e2e 直接红：`$ is not defined`）。
   非单词字符标识符必须特殊处理。
3. `state = s` 整体重绑定（refresh()）——对象属性可跨模块变更，
   重绑定不行；`state` 因此入桥。

### 6.4 验收证据（M5 第一阶段）

| 验收项 | 证据 |
|---|---|
| 行为 | e2e 10/10（对 ESM 产物）：契约校验、播放推进、步进、三面板、/api/state、控制台零 error |
| I-1 | 托管浏览器断言：播放中 playhead transform = `translate3d(470px,0,0)` 且 JS 不重复写（合成层插值），时间码独立推进 |
| 单测 | 23/23（直接 import 源分片；store 桥在 node 下同样生效） |
| 门禁 | `npm run gate` 全绿；`gate_local.sh --jobs web` 全绿 |
| 产物 | 99555 字节（minify -33%），freshness 一致 |

## 7. M5 后半段 + M6：dialog 组件化、平台特性、a11y/安全（2026-11-17）

### 7.1 组件化：原生 `<dialog>` 取代手写模态层

- **imp-panel → `<dialog>` + `showModal()`**：独立 mask div 删除，
  `::backdrop`（半透明 + blur）由平台承担；焦点圈定、Esc（cancel→close
  原生链路）、backdrop 点击关闭全部是平台行为。close 事件同步 `.open`
  class（e2e 兼容信号）。
- **exp-panel → `<dialog>` + `show()`**：非模态——保留"开着导出面板仍可
  操作预览"的既有行为（`showModal` 会引入 inert 阻塞，属行为变更，
  不做；若产品层需要阻塞，改一行即可）。
- **ask-panel 保持 fixed div**：锚定式侧栏面板，语义上不是模态，
  强套 dialog 无收益。
- 截图验证：面板完整渲染、遮罩 + blur 正常、深色主题一致、
  无 UA 默认样式残留（`margin: 0; padding: 0; color: inherit` 重置）。

### 7.2 store 订阅底座

`store.js` 新增 `watch(key, fn)`：accessor setter 触发被动通知。
播放组（I-1）不订阅——通知无订阅者时零行为变化，组件化只剩
渲染层职责搬迁。

### 7.3 平台特性（mwg 对齐项落地）

| 特性 | 落点 |
|---|---|
| `structuredClone`（Baseline 2022） | splitSceneData 深拷贝、inspector 场景复制（替换 JSON.parse(JSON.stringify)） |
| `prefers-reduced-motion` | base.css 全局压动效 + playhead.js 主线程兜底推进形态（仅 reduced-motion 用户） |
| `<dialog>` + `::backdrop` | imp/exp 模态（见 7.1） |

### 7.4 a11y 与安全

- **aria**：39 个带 title 的按钮补 `aria-label`（图标按钮可被屏幕阅读器
  命名）；无 title 按钮全部有可见文本（内容即可访问名），无需补。
- **postMessage 收紧**：4 个发送站点 `targetOrigin` 从 `"*"` 钉到
  `location.origin`；接收端（shortcuts.js message handler）加同源校验，
  跨源消息一律丢弃。

### 7.5 M7：e2e 入 CI

CI `web` job 扩展为两段：hermetic gate（不变）+ 真实链路 e2e
（起 hf_studio --serve → gallery spec 合成契约 → Playwright Chromium
跑交互基线，控制台零 error 为硬断言）。

### 7.6 M5/M6 完成后的状态

- 22 个分片全部 ESM；store 桥 + 订阅底座就位；两个模态为原生 dialog。
- **M5 剩余**：timeline/inspector/transport/scene-card 的 Custom Elements
  化——这些与渲染主循环深度纠缠，收益/风险比低于已完成项，
  按 ADR 原则（触发条件见 PLAN §2.3）延后，待手写 store 出现明显
  bug 率或团队扩容时再评估。
- **M6 剩余**：`:has()`/View Transitions 等渐进增强项（additive，
  待 UI 稳定后逐项落地）；Trusted Types/更严格 CSP（独立小项，过 ADR）。
