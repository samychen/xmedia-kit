# xmedia-kit AI 视频生成管线

Agent（或人类）填 spec JSON → 生成器出契约 HTML → html_bake 烤 MP4。
三步管线，确定性管线（同 spec 双烘逐帧 md5 差异仅启动区 2 帧——
2026-11-18 起 GPU 合成根治了 software compositor 的 tile 缺失，见
「确定性边界」）。

返回：[README](../README.md) ｜ 相关：[EXAMPLES](EXAMPLES.md) ｜ [COOKBOOK](COOKBOOK.md)

### 确定性边界（2026-11-17 取证 / 2026-11-18 GPU 合成更新）

管线各层的确定性实测结论——**输入侧确定，呈现层经 GPU 合成已收敛**：

| 层 | 确定性 | 依据 |
|---|---|---|
| hf_tts（say 离线） | ✓ 幂等 | 同文本复用缓存产物，不重生成 |
| hf_compose（spec → HTML） | ✓ 逐字节 | 同 spec 两次 compose 的 HTML md5 一致（canonical sha256 锚定） |
| 音频混音 | ✓ 帧级 | 双烘 `-map 0:a -f md5` 一致 |
| 页面 GSAP 时间线 | ✓ 逐 seek | seekDone 回执探针：`tl.time()` 每帧 = 请求 t + 1ms（seek 脚本的 `totalTime(t+1e-3)` 设计值），两支烘焙完全一致 |
| 视频像素 | ✓ GPU 合成下双烘 diff 2/165（仅启动区竞态） | 2026-11-18 见下 |

**视频像素差异的根因（已根治）**：曾经 macOS OSR 走 software compositor
（`disable-gpu`，历史注释称 GPU 子进程在 bundle 外 SIGSEGV——**已过时**：
html_bake.app 现在带完整 Helper.app 结构，GPU 子进程实测正常）。software
compositor 的 partial raster 下，**内容变化帧**（字幕点亮/场景进出/动画）
触发整层重光栅，OnPaint 抓帧时部分 tile 尚未光栅完成 → tile 级黑块。

**修复（2026-11-18）**：`cef_app.cc` 移除 `disable-gpu` +
`disable-gpu-compositing`，启用 GPU 合成——实测 165 帧烘焙无崩溃、
双烘逐帧 md5 差异从 12–18 帧降到 **2 帧（仅启动区竞态，中段动画帧全
一致）**，tile 缺失消失。OSR 的 OnPaint BGRA 回读路径在 GPU 合成下正常。
取证记录（software 时代的假设已被推翻）：seek 在飞门控丢弃未触发、
GSAP 时间线逐 seek 一致、无动画 spec 同样出现（差异与动画无关）、
compose 产物逐字节一致——差异确实只在呈现层，呈现层换 GPU 后问题消失。

遗留：启动区 2 帧差异（首帧 paint 与采集脚本注入的竞态家族，
量级远小于此前，仍属已知边界）。

### VT 时钟专项侦察（2026-11-17，未合入；GPU 合成后 seek 时钟已够用，
### 专项优先级降低，留档仅在需要 VT 特性时启用）

`--clock virtual_time` 对 HF 契约页**从未可用**（架构事实 + 三层黑盒），
实验性修复均已回退，此处留档供专项续作：

1. **时间线缺口**：`TickBakeClock` 的 VT 分支只发 VT 步进，从不 dispatch
   `MakeSeekScript`——HF 契约页的 paused GSAP 注册表不被虚拟时钟推进
   （GSAP ticker 走真实墙钟），页面停在 bootstrap，整段输出纯背景。
   修法（已验证可行）：VT 分支合体 dispatch seek JS——TLTRACE 探针实测
   165/165 seek 执行且 `tl.time()` 逐帧精确。
2. **管线层级与预算**：seek JS 执行 ≠ 画面更新。renderer 全管线
   （JS → style recalc → paint → commit → raster → activate）各层需要
   VT 预算；同拍 BeginFrame 合成到的是旧样式快照（画面卡 bootstrap）。
   补一个步进后内容出现但 raster 未完成（tile 黑块）——正确修法是
   **加大单步 budget**（40ms 的 mix 对齐值不够）而非逐层补步进
   （补步进会被 VT 状态机的 credit/C2 重试交互打乱，实测 303≠495）。
3. **启动竞态（未解）**：`CreateBrowserSync` 后立即 arm VT（DevTools
   `setVirtualTimePolicy`），间歇性 ~50% 概率 budgetExpired 从不到达
   （0 次 expire）→ 页面 load 不完成 → `no first frame in 15s`；成功
   实例间行为也不同（首帧后 pre-apply ack 全丢/烘完退出挂起/正常退出
   三种）。load-end 兜底 Invalidate+BeginFrame 无效（OnLoadEnd 本身没到）。
   疑点：DevTools agent 就绪前 `ExecuteDevToolsMethod` 静默丢弃。
   需要对齐 MC 上游 VT 实现或 CEF 升级后重估。

## 快速上手

**统一 CLI**（推荐，所有能力的入口；详细子命令 `xmedia --help`）：

```bash
python3 tools/xmedia.py init my-proj          # 脚手架：可运行示例 spec
python3 tools/xmedia.py render my-proj/demo.json --out my-proj/demo.mp4
                                             # 一条命令全链：TTS→compose→
                                             # validate→bake（参数全自动推导）
python3 tools/xmedia.py doctor               # 环境自检（8 项逐项报告）
```

单步命令（等价透传）：`xmedia tts/compose/validate/studio/sfx`。

**叙事计划（Storyboard）**：商业叙事的节拍骨架，先给计划再落地
（HF prompting/design-systems 的叙事纪律）：

```bash
python3 tools/xmedia.py storyboard list          # 5 个商业 Arc
python3 tools/xmedia.py storyboard problem-solution \
  --message "把切片发布从半天缩短到一分钟「切片工作流」" \
  --audience "短视频团队" --mood "轻快电子" -o sb/spec.json
# 5 帧骨架（Hook→问题→方案→证明→CTA），每帧带 type/persuasion/beat
# 叙事徽标（Studio 分镜卡显示）；占位文案在分镜卡/Ask AI 里改
```

**能力对齐 HF Studio 的评估结论**（2026-11-17 首评；2026-11-18 增补：Home/叙事 Arc/双屏对照/内联编辑）：

- **云渲染（deploy/cloud、Lambda/Cloud Run）**：评估暂不实现——本仓库
  定位是本地确定性引擎（单机烘焙 + 硬件加速真零拷贝路径），云渲染是
  新基建（队列/存储/计费/隔离），依赖真实多租户需求再立项；4K 本地
  烘焙已有压力测试覆盖。
- **HDR / Motion Blur / VFX chain（data-vfx-chain）**：评估暂不实现——
  shader transitions 已覆盖大部分视觉过渡；HDR 编码管线与 per-pixel
  效果链属于渲染核心扩展，待渲染确定性专项收尾后评估。
- **HeyGen 生态绑定项**（Avatar presenter / HeyGen Video 1 / HeyGen
  Voice / Share for review 云评审）：明确不做——依赖 HeyGen 账号体系
  与云服务，与本仓库本地/开源定位冲突。替代路径：口型播报可接开源
  TTS+口型方案；评审分享用产物文件 + 常规渠道。

**Studio app 文档站四页对齐评估**（2026-11-17，对齐
hyperframes.heygen.com/studio-app 及其 /first-video、/reference-video、
/brand-kits 三页；读片/网站扫描等云侧能力依赖 HeyGen 账号体系，结论
沿用上节"明确不做"）：

| HF Studio app 能力 | xmedia-kit 现状 | 差距 / 替代路径 |
|---|---|---|
| Studio app 桌面应用（Electron，描述→agent 建片→改） | `hf_studio.py --serve` 本地浏览器 UI（Ask AI 对话 + 时间轴 + 预览，Playwright e2e 10 项） | 形态差异：浏览器版 vs 桌面版；CEF 壳可选 |
| Studio app Home：项目管理（一句话 + Arc + 受众/音乐 → 建片）+ chat 主界面（agent 主导，Framey 引导） | **已支持**：`hf_studio.py --serve --home`——项目管理页（一句话 + Arc 下拉[5 个商业叙事 Arc] + 受众/音乐 → `/api/project/create` → hf_storyboard 骨架 → 项目目录 spec+contract → 项目列表 `/api/projects`，存 `~/.xmedia/projects/`）；chat 主界面化已升格：**对话面板常驻左栏第一面板**（默认 active，五面板 对话/场景/脚本/分镜/源码，fab 浮层移除，Ctrl/⌘+K 聚焦输入）；**agent 后端通用化**：`XMEDIA_AGENT_CMD`（JSON 数组/shell 串）接入任意 agent CLI（CodeBuddy/DeepSeek/aider 即插即用，XMEDIA_AGENT_NAME 显示名），CodeBuddy 已实测接入；**规则引擎 8 类高频指令**（副标题/场景色/转场/动画/插场景/模板场景/BGM 音量） | 部分：灵感 feed 不做（云社区） |
| first-video：一句话→提问→storyboard 审批→成片 | **storyboard 已支持**：Studio 左栏「分镜」页签逐镜审阅——每镜**真实渲染帧**（/api/sb-frames：html_bake 低分辨率全段烘焙 → 场景中点抽帧，video 场景走连拍网格）+ 时机 + 文案，点画面选中联动 Inspector/时间轴/预览（不跳走），「改这镜」一键定位，卡上内联编辑文案/时长（应用走重组）；+ `launch` 四段式 + Ask AI 待应用队列 | 渲染前逐镜审阅闭环完成 |
| Storyboard 工作流（prompting/design-systems）：叙事计划 → 逐帧落地 | **本地版已支持**：`xmedia storyboard <arc>`——商业叙事 Arc 模板库（launch / problem-solution / listicle / explainer / comparison），每帧带 HF 叙事词汇（type=persuasion=beat：hook/social_proof/cta · before-after/counterexample/callback · aha/resolve…）；`--message/--audience/--mood` 四要素产出骨架 spec + `spec.storyboard` 段（方向块：两色纪律/breather/负面清单——供 agent 模式读）；Studio 分镜卡显示叙事徽标 | HF 的 plan→build 由 agent 逐帧生成；本地版是确定性骨架（占位文案手工/agent 填），叙事纪律已**注入 agent prompt**（spec.storyboard.direction 存在时方向块进 agent 上下文，逐帧编辑必须遵守）；compose/validate 层为 warning 级校验（hf_validate 两色/breather/负面清单机器可查子集） |
| first-video 卡片：四部分 + 网站扫描（读字体/色/logo） | `launch --hook/--proof/--turn/--cta --url`（`--url` 尽力抓网站**文案**） | 网站品牌扫描（色板/字体/logo 文件）不做；brand 走手工 CLI |
| reference-video：读参考视频→复刻节奏/字幕，换人换词换品牌 | **本地弱化版已支持**：`xmedia reference <视频>`——ffmpeg 场景检测切镜头（保留剪辑节奏，最短镜并入/超限合并）、逐镜缩略图对照表、同节奏 spec 骨架、可选 --asr 全局字幕段；Studio 内说「参考 <路径> 讲 X」同样触发（分析 → 同节奏骨架 → 主题填充）；预览工具条「对照」= 双屏同钟对照（参考视频与成品同播放头，偏差 > 0.25s 自动对齐，`/api/reference/video` 登记外部素材白名单 + asset 响应支持 Range/206）；不复刻视觉特效/换脸（云能力，见上节） |
| brand-kits：品牌套件（色/字体/logo/文件，可从网站扫） | **logo 图片入包已支持**：`brand save --logo-image <文件>`（复制进品牌 assets）+ `brand scan` 自动下载 og:image + `brand apply`（`--logo-mode corner/backdrop`）注入每个无图场景（hf_compose 场景级 `image` 字段，`scene-image-corner/backdrop` CSS）；字体族经 `--font-family` 写 `spec.fonts.body`；+ 网站扫描（色板/对比文字色） | 部分：无 workspace 团队共享 |
| brand-kits：workspace 团队共享 | 不做（本地单人定位） | — |
| 灵感 feed / remix 社区 | 不做（云社区） | — |
| HeyGen presenter / Pro credits 读片计费 | 不做（上节"明确不做"） | — |
| project 模型 + Undo | `project.json` + run Undo（20 轮） | ✓ 对齐 |
| 模板 catalog | `templates/` 19 个场景模板（同款 slots 协议）+ **Studio「模板」tab**：卡片网格（名称/描述/slots/一键插入场景，走 Ask AI 规则引擎 → 重组 → 分镜真实帧刷新） | 数量少于 HF catalog（449 页索引） |
| 媒体导入 / 字幕 | `hf_import` 零拷贝 + `hf_asr` SRT 分段字幕 | ✓ 对齐 |
| 导出（后台渲染 + 取消） | `/api/bake` + 取消，产物命名对齐 HF | ✓ 对齐 |

**测试覆盖现状**（2026-11-18 盘点）：Home/sb-frames/双屏对照/内联编辑/叙事 Arc
等新能力目前由**人工浏览器验证 + 提交信息记录**，尚无自动化断言——e2e
12 项（含 storyboard 卡数/定位链）与 hf_ai/studio smoke 未覆盖新面；
`hf_studio_smoke` 在 Linux 环境性失败（`say` TTS macOS 专用），跨平台
需 http provider 或无旁白形态。补自动化是当前质量侧最大缺口。

**launch 四段式发布视频**（对齐 HF first-video：Hook → Proof → Turn → CTA）：

```bash
python3 tools/xmedia.py launch \
  --hook "还在手写视频脚本？" \
  --proof "1036|回归测试全部通过" \
  --turn "确定性管线，同 spec 同画面" \
  --cta "立即试试" \
  --url https://你的网站 --out launch/launch.json
# 产物直接 xmedia render launch/launch.json --out launch.mp4
```

**品牌套件**（brand kit，存储 `~/.xmedia/brands/<名>.json`，Studio 内说
「应用品牌 <名>」同样生效）：

```bash
python3 tools/xmedia.py brand save acme --palette1 "#10101e" \
  --palette2 "#182138" --text "#e8ecff" --voice "Tingting" \
  --outro "由 ACME 出品" --logo "ACME"
python3 tools/xmedia.py brand apply acme --spec launch/launch.json
```

手动分步（与统一 CLI 等价）：

```bash
# 1. 写 spec JSON（agent 或人类填空）
cat > my_video.json <<'EOF'
{
  "composition": {"id": "demo", "width": 1920, "height": 1080, "fps": 30},
  "background": "#0a0a1a",
  "scenes": [
    {"type": "title", "text": "产品发布", "subtitle": "v2.0",
     "duration": 3, "bg": "#0a0a1e", "text_color": "#ffffff",
     "animation": "fade_in_up"},
    {"type": "text", "text": "核心特性",
     "duration": 4, "bg": "#16213e", "text_color": "#e0e0e0",
     "animation": "slide_in_left",
     "content": [{"text": "• 确定性渲染"}, {"text": "• 音频混音"}]},
    {"type": "outro", "text": "Thanks",
     "duration": 2, "bg": "#0f3460", "text_color": "#ffffff",
     "animation": "fade_in"}
  ],
  "audio": {"src": "bgm.mp3", "volume": 0.6, "fade_in": 1, "fade_out": 2}
}
EOF

# 2. 生成契约 HTML
python3 tools/hf_compose.py my_video.json -o my_video.html

# 3. 验证契约合规（0 errors 才交给 html_bake）
python3 tools/hf_validate.py my_video.html

# 4. 导出 MP4（--url 支持相对路径；--seconds 取 spec 总时长）
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url ./my_video.html \
  --out my_video.mp4 \
  --seconds 9 --fps 30 \
  --width 1920 --height 1080 \
  --clock seek
```

### 旁白驱动 + 竖屏（AI-auto-generate-video 模式）

`hf_tts.py` 逐场景生成旁白并回填 `narration`，`hf_compose` 拟合时长并把
旁白发成 `<audio>` 元素进确定性混音；`format: "9:16"` 一键竖屏：

```bash
# 1. TTS 生成旁白（voice/scene-N-<文本哈希>.wav + 回填 narration →
#    demo.narrated.json；词变自动重生成，跨 spec 同目录不串音）
python3 tools/hf_tts.py demo.json

# 2. 拟合时长 + 生成契约 HTML
python3 tools/hf_compose.py demo.narrated.json -o demo.html

# 3. 验证 + Studio 预览（浏览器打开 demo.studio.html 边看边调）
python3 tools/hf_validate.py demo.html
python3 tools/hf_studio.py demo.html

# 4. 导出（9:16 → --width 1080 --height 1920）
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url ./demo.html --out demo.mp4 \
  --seconds <总时长> --fps 30 --width 1080 --height 1920 --clock seek
```

可直接复制的 `demo.json`（旁白文本即场景文本，TTS 只认 `text`）：

```json
{
  "composition": {"id": "demo", "format": "9:16", "fps": 30},
  "background": "#0a0a1a",
  "tts": {"provider": "say", "voice": "Tingting", "rate": 200},
  "scenes": [
    {"type": "title", "text": "欢迎使用 xmedia-kit",
     "subtitle": "AI 视频生成管线", "bg": "#1a1a2e",
     "animation": "fade_in_up"},
    {"type": "stats", "value": "1026", "description": "回归测试全部通过",
     "bg": "#16213e", "animation": "scale_in"},
    {"type": "outro", "text": "谢谢观看", "duration": 2, "bg": "#0f3460"}
  ]
}
```

`--seconds` 取 `hf_compose` 打印的总时长（旁白拟合后可能 ≠ 手写 duration
之和）。spec 侧也可以手写 `narration` 路径跳过 TTS（时长照样拟合）。场景
时长 = ffprobe(旁白) + padding，旁白 `<audio>` 与场景窗口同起点进混音，
BGM（默认 0.6 音量）衬在旁白之下。

### xmedia Studio（本地服务，浏览器编排）

```bash
python3 tools/hf_studio.py demo-output/demo.html --serve \
  --spec demo-output/demo.narrated.json --port 8120
# 浏览器打开 http://127.0.0.1:8120
```

三栏工作区（对位 Remotion Studio 的编辑→预览→渲染闭环）：

| 区域 | 内容 |
|---|---|
| 左 | 场景列表（点选、上下移序、时长与旁白标记）与「源码 JSON」页签；SFX 开关 |
| 中 | 预览区：原生画布等比缩放、传输条（含声音开关与音量）、校验状态 |
| 右 | 场景属性面板：内容字段（按场景类型或自定义模板动态生成）、样式（背景 / 文字色 / 动画）、时序（时长 / 转场）、应用 / 复制 / 删除。**转场与入场动画选完即生效**（下拉 `change` 直接应用并重组），其余字段仍走「应用到场景」 |
| 下 | NLE 风格五轨时间轴（场景 / 字幕 / 旁白 / 音效 / 音乐）+ 时间轴工具栏与可折叠日志 |

**预览区**（`tools/studio_ui.html`）：iframe 始终以 spec 的原生像素尺寸渲染，
再经 CSS `transform: scale()` 等比缩放——不裁切、不失真，与 `html_bake` 的
`--width/--height` 同源。工具栏提供适配与 25%–300% 缩放、棋盘 / 深色 / 浅色
背景、三分构图网格、10% 安全区与全屏；传输条提供跳首 / 逐帧 / 播放暂停 /
跳尾 / 循环 / 0.5×–2× 速率、帧级 scrubber 与 `mm:ss.cc · 帧 N/M · fps` 时间码。
点选预览画面或时间轴场景块会双向联动场景列表与属性面板；重组重载预览后自动
恢复到重组前的时间点。打开时默认选中第一个场景，并定位到该场景的稳定帧——
契约页的入场动画在 t=0 时 `opacity≈0`，直接停在 0 帧只会看到一块空背景；
「跳到开头」按钮可随时回到 0 帧。

**预览声音**：契约页的 `<audio data-start/data-duration/data-volume>` 是导出侧
的确定性混音清单，本身不带 `autoplay`，直接用浏览器打开契约页是静音的。
Studio 注入的驱动把这些元素当作多轨素材：播放头进入素材窗口即按对应偏移
起播，离开窗口或暂停即停；拖动时间轴强制对位，0.5×–2× 变速同步
`playbackRate`。**播放期间绝不逐帧 seek**：每次 `currentTime` 赋值都会让媒体
重新缓冲并短暂静音，用 rAF 累加的时间去校正音频时钟会形成「落后 → seek →
再缓冲 → 再落后」的循环，正是声音断续的根因。

**播放头时钟**：视觉时间由 rAF 严格单调推进，正常播放时步长恒为 `dt × rate`，
音频时钟**不参与逐帧纠偏**。两种早期做法都会造成边界卡顿：直接把播放时间
覆盖成 `audioClock()`，会在音轨起播 / 切换时把游标钉在素材起点（新轨缓冲中
`currentTime=0`）；把音频时钟当作每一帧的目标做牵引，则会在新 SFX 起播、
音频时钟短暂滞后时连续多帧把步长压到下限（60% 速度），观感是「第二、第三个
音效开始时顿一下，之后正常」。现在只有偏差超过 0.18s 死区才以 ≤2ms/帧
缓慢收敛，参考时钟也只取确实已推进（`currentTime > 0.02`）且与当前时间
最接近的一条轨，所有音轨边界播放头都匀速前进。此外：页面加载后把所有
`<audio>` 静音预热一次（提前初始化解码器与音频输出设备），播放态的音频同步
移到 `setTimeout(0)` 任务、状态上报提前到 `drive()` 开头，减少第 2、3 个
SFX 首次 `play()` 对渲染帧的占用。

**游标合成层动画**：Safari 的同源 iframe 与父页共享主线程，音轨首次 `play()`
仍会同步阻塞父页 rAF（录屏实测 `00:01.37` 前连续 13 帧、约 216ms 游标不
动）。因此播放时父页**完全不写** `transform`：`startPhAnim()` 给播放头挂一条
`transform <剩余时长>s linear` 的 CSS transition，配合 `will-change: transform`
由 compositor 独立插值，主线程被占满时游标仍匀速前进；`phFrame` 只按 1/4
频率更新时间码、场景标签与自动滚动，状态文本仅在变化时写 DOM。过渡只在播放
开始、暂停、循环回卷、显式定位、倍速变化、时间轴重建时重建；播放中绝不用
iframe 上报时间拉回游标——iframe 的 rAF 同样会被阻塞，它的时间会落后墙钟。

**预览单音轨混音**：服务模式在首次 `/preview` 请求时用 ffmpeg 把契约页所有
`<audio>` 与 `<video>` 按 `data-start` / `data-volume` / `data-media-start`
预混成一条 WAV（`/__xmedia_preview_mix__.wav`，按契约页内容哈希缓存、支持
Range 206，重组后自动重算）。预览页只播放这一条音轨，**播放中途不存在任何
元素首次 `play()`**，从源头消除 Safari 的解码器 / 输出设备同步初始化开销。

`<video>` 也要进这条混音：视频元素本身带 `muted`（画面由它播，声音统一走
混音），导出侧 `html_bake` 一直把 `is_media` 元素（含 video）叠进成片音轨；
预览若只认 `<audio>`，导入实拍素材的工程在 Studio 里就是「有画面、完全没
声」。素材原声按 `data-media-start` 先做输入侧 `-ss` 定位、再按
`data-duration` 修剪、最后 `adelay` 到场景起点，与画面取帧同源——切开过的
工程不会出现「声音比画面早/晚一段」。不想保留原声可写
`data-has-audio="false"`（该轨整条跳过）。混音末端不补 `apad`（ffmpeg 7 上
`apad` + `adelay` 的 framesync 会挂死），改用输出端 `-t` 总时长收口。

契约页原有的音轨仍完整保留给 `html_bake` 离线混音，仅在预览时禁用预加载；
ffmpeg 不可用时自动降级回多轨引擎。混音音量、起始时间与导出产物一致。

传输条的喇叭按钮（`M`）静音、滑杆调总音量，音量沿用契约页
的 `data-volume`；浏览器拦截自动播放时会出现「点此开启声音」提示，任意用户
手势会自动重试解锁。内置 SFX 在契约页中写的是绝对文件系统路径（供 `file://`
导出解析），HTTP 预览下由服务的 `/__xmedia_root__/` 只读映射提供——只在
`/preview` 的内存副本里改写，契约文件与导出链路不受影响。

**时间轴**（`tools/studio_ui.html`）：对位专业 NLE 的呈现——150px 轨道头
（名称 / 副信息 / 眼睛可见性开关 / 音量值）、带主次刻度与时间码的标尺、
蓝色场景片段内的连续缩略图 filmstrip、橙色字幕条、绿色旁白双通道波形与
SFX / BGM 块状波形、贯穿所有轨道的红色播放头。缩略图按场景 `bg` 与类型、
波形按素材路径哈希**确定性生成**，不引入随机数或外部素材；有 `video` 的场景
用 ffmpeg 抽 480×270 帧，按该片段自己的源内窗口（`video_from` + 时长）均匀
取样，切割后前后两段的 filmstrip 不再雷同。

画布按 `px/s` 绘制并可横向滚动：`适配 / + / -` 与 Ctrl/⌘ + 滚轮缩放
（12–600 px/s），主刻度间隔随缩放自适应；拖动标尺定位默认吸附到帧与场景
边界，工具栏可关闭吸附。点选场景片段联动左栏与属性面板，拖右缘裁剪时长，
选中后用 `‹ ›` 调整顺序；播放时播放头自动跟随滚动，轨道头眼睛可隐藏该轨。

**区间选择（in / out）**：`Shift` + 拖拽（或点工具栏「区间」后直接拖拽）
在时间轴上框出一段——**选区外压暗、选区内保持原色**，标尺上叠一条蓝色
区间带，两端蓝色把手可继续拖动微调；`I` / `O` 以播放头打点，`Esc` 或
`✕` 清除。工具栏实时显示 `00:05.00 – 00:20.00 · 15.00s · 450 帧`，
`▶区间` 从 in 播到 out 自动收住（预览驱动 `setRange({a,b,loop})`），
`循环` 让播放到 out 回卷到 in。区间只约束预览播放，不改 `spec`；
破坏性剪辑走单片段拖右缘与下方的**片段切割**。

**片段切割**：把播放头停在刀口，按 `S` / 点工具栏「切割」/ 右键片段，选中
（或播放头所在）的那一段就切成前后两段——前段收在刀口，后段继承类型、
样式与文字继续到原终点。视频场景的后半段会写入 `scenes[].video_from`（源
视频内起点 = 原 `video_from` + 刀口偏移），`hf_compose` 把它发成
`<video data-media-start>`（单位与 `data-start` 一致：秒），导出侧的视频原生
层按该时刻取帧、确定性混音与 Studio 预览混音按同一时刻修剪音轨——等于把一条
素材物理切开，不是复制同一段。缩略图同样按各段的源内窗口抽帧，所以切开后两段的
filmstrip 立刻不同。旁白拟合时长的片段（有 `narration`）拒绝切割；
`captions.segments` 是绝对时间轴字幕，切割不改总时长，故字幕轨不动。

接缝固定为**硬切**：后段被显式写成 `transition: "none"` + `animation: "none"`。
两段本是同一画面的连续帧，若继承原来的入场动画，后半段会在刀口再淡入
一次（画面内容没变，看起来就是「转场没反应」），而继承转场还会让总时长
被重叠窗吃掉 0.5s。要在这个位置做过渡，切完在属性面板给**后半段**选一个
转场即可（选完立即生效，不必再点「应用到场景」）。

> 一个物理事实：同一段素材前后两帧几乎一样，`fade` 交叉淡化在这种接缝上
> 本来就看不出来（两边画面相同，混合前后一样亮）。想在刀口做出可见效果，
> 用 `slide-left` / `slide-right` / `wipe`，或先把两段的内容裁开。

| 快捷键 | 作用 |
|---|---|
| `Space` | 播放 / 暂停 |
| `S` | 在播放头处切割片段（⌘/Ctrl+S 不拦） |
| `M` | 预览声音静音 / 取消静音 |
| `←` / `→` | ±1 帧；`Shift` + 方向键 ±10 帧 |
| `Home` / `End` | 跳到开头 / 结尾 |
| `I` / `O` | 以播放头设区间 in / out |
| `Esc` | 清除区间 |
| `+` / `-`、Ctrl/⌘ + 滚轮 | 缩放档位切换 |
| `Ctrl/Cmd + Enter` | 重组合成 |

服务模式下内嵌的悬浮传输条自动让位（`/preview?embed=1`），由父页统一控制；
单文件注入模式仍保留自带的悬浮传输条。spec 文件即落盘产物——重开服务或直接
走 CLI 导出都接得上。

## 功能速查（功能 → 命令）

| 功能 | 命令 / spec 字段 |
|---|---|
| 场景模板（7 类） | `hf_compose`，spec `scenes[].type`：`title`/`text`/`outro`/`quote`/`list`/`stats`/`comparison` |
| 片段切割 / 源内起点 | Studio 时间轴 `S`（或工具栏「切割」/ 右键片段）→ 后写入 `scenes[].video_from`（秒，源视频内起点）→ `<video data-media-start>` → 导出侧按该时刻取帧、混音按同一点修剪音轨 |
| 入场动画（6 种 + none） | `scenes[].animation`：`fade_in`/`fade_in_up`/`fade_in_down`/`slide_in_left`/`slide_in_right`/`scale_in`，`none` = 硬切进入 |
| 画幅预设 | `composition.format`：`16:9` / `9:16` / `1:1` / `4:5`（覆盖 width/height） |
| TTS 旁白 | `hf_tts.py <spec.json> [-o narrated.json] [--provider say\|http] [--voice NAME] [--rate N] [--endpoint URL] [--out-dir DIR]`；spec `tts` 段；环境变量 `TTS_PROVIDER`/`TTS_ENDPOINT`（兼容 `OMNIVOICE_ENDPOINT`）；幂等（产物可探测即跳过） |
| 旁白驱动时长拟合 | `scenes[].narration` + `padding`（场景时长 = ffprobe 实长 + padding，GSAP 游标与 clip 窗口同源）；旁白音量 `scenes[].narration_volume` |
| BGM | spec `audio: {src, volume, fade_in, fade_out}`（默认 0.6 音量衬在旁白之下） |
| 分块导出（逐场景幂等重渲） | `html_bake --chunks N`（每块独立编码器冷启 + Concat 无损拼接） |
| 导出时钟 | `--clock seek`（页面契约，帧级精确，默认推荐）/ `virtual_time`（VT 步进）/ `live`（墙钟） |
| 编码器 / 封装 | `--codec mjpeg`（默认，基础构建）/ `libx264`（x264 依赖层开启时）；输出封装按扩展名 `mp4`/`gif`/`webm`/`mov` |
| Lottie 叠加 | `html_bake --lottie <path> [--lottie-z N] [--lottie-speed S] [--lottie-loop N]`（z 垫在 HTML 元素之下，bake 时钟驱动） |
| 页面数据注入 | `html_bake --data '<json>'`（`window.update(d)` 契约，首帧后注入一次） |
| 嵌套合成 + 变量 | spec `content` 手写 `data-composition-src` / `data-variable-values`（注入 runtime 页内挂载，子合成经 `__hyperframes.getVariables()` 消费） |
| 元数据导出 | 自动产出 `<out>.html_elements.json`（元素时序/矩形，毫秒域；调试与合成工具消费）。`video`/`audio` 元素额外带逐帧渲染态 `opacity`（自身 × 祖先）与 `clip`（祖先 `clip-path(inset)` 交集）——转场 / 入场动画就是靠它落到视频原生层上的 |
| 契约自查 | `hf_validate.py <html> [--json]`（composition 根/duration/clip/__timelines/GSAP/file:// 子资源告警——agent 无人值守的前置门） |
| 原版契约兼容 | 生成器自动保证：`data-composition-id` 根 + clip `data-start/duration`(秒) + paused GSAP 注册 `window.__timelines` + 内联 GSAP（离线确定性）；驱动 = 注册表 `totalTime(秒+1e-3)` + 采集端 `data-time-unit="s"` 秒→毫秒归一（契约对齐 heygen-com/hyperframes） |
| 确定性音频混音 | 页面 `<audio>`/`<video>` 媒体元素（data-start/duration/volume/fade-in/out，video 另用 media-start 修剪源内起点）→ libav 解码 → AudioConverter 统一重采样 48k FLTP → media-start 修剪 → 增益/斜坡 → data-start 叠加 → AAC 192k |
| SFX 音效 | 三级选择（场景 `sfx` 覆盖 → 文本关键词 → 类型默认）随场景起点叠加，默认音量 0.4；素材 = `assets/sfx/` 五个 ffmpeg 合成音效（whoosh/pop/ding/success/transition，零授权，生成命令见该目录 README）；关闭：`--no-sfx` 或 spec `"sfx": {"enabled": false}`；自定义目录 `sfx.dir`、音量 `sfx.volume` |
| Studio 预览区（服务模式） | 原生画布等比缩放（不裁切）+ 适配 / 25%–300% 缩放 + 棋盘 / 网格 / 安全区 / 全屏 + 传输条（播放、逐帧、循环、倍率、帧级时间码）+ 预览点选联动，见下方 Studio 小节 |
| Studio 预览 | 注入模式：`python3 tools/hf_studio.py out.html` → 注入传输条（播放/拖动/场景跳转/空格与方向键）的 `<stem>.studio.html`，浏览器打开即预览；幂等原地更新 |
| xmedia Studio 服务 | `python3 tools/hf_studio.py out.html --serve --spec spec.narrated.json [--port 8120]` → 浏览器 UI（127.0.0.1）：**spec 编辑器热重组**（Ctrl/Cmd+Enter）、五轨时间轴点选与标尺定位、**区间（in/out）选区与区间播放**、TTS 一键、**导出按钮（尺寸/时长由 spec 自动推导）**、校验与日志面板——对位 Remotion Studio 的编辑→预览→渲染闭环 |
| ASR 分段字幕 | `xmedia asr <media\|.srt\|.vtt> -o captions.json [--provider auto\|srt\|whisper\|fw]`；输出 `{segments:[{start,end,text}]}`，填进 spec `captions.segments` |
| 素材自动导入 | `xmedia import <dir> -o spec.json [--asr] [--captions cap.srt] [--endcard] [--format 9:16]`；ffprobe 实长按文件名排序排时间轴 |
| 自然语言编辑 | Studio 左栏「对话」主面板（Ctrl/⌘ + K 聚焦输入）或 `POST /api/agent`；规则引擎零配置（8 类高频指令），agent 走 `XMEDIA_AGENT_CMD` 任意 CLI / OpenAI 兼容 LLM |
| MCP server | `xmedia mcp --studio-url http://127.0.0.1:8120`（stdio JSON-RPC，11 个 tools） |

### TTS provider（hf_tts.py）

| provider | 说明 | 依赖 |
|---|---|---|
| `say` | macOS 离线 TTS（`--data-format=LEI16@22050` 出 WAV）| 系统自带，零依赖 |
| `http` | 本地 TTS 服务：`POST {text, voice, rate}` → audio 字节（OmniVoice 式，对齐 AI-auto-generate-video 的本地服务架构）| 自备服务 |

```json
"tts": {
  "provider": "say",
  "voice": "Tingting",
  "rate": 200,
  "endpoint": "http://127.0.0.1:8123/tts",
  "out_dir": "voice"
}
```

CLI `--provider/--voice/--rate/--endpoint/--out-dir` 覆盖 spec；优先级
CLI > spec > 环境变量（`TTS_PROVIDER` / `TTS_ENDPOINT`，兼容
`OMNIVOICE_ENDPOINT）> 默认。幂等：产物已存在且 ffprobe 可探测则跳过，
重复执行安全；可朗读文本 = `text` 优先，缺失时按类型聚合专有字段
（stats 取 value/description，quote 取 quote/author，list 取 items，
comparison 取列标题）；已有 `narration` 的场景跳过（保留既有旁白）。
默认输出 `<stem>.narrated.json`。

**交付产物**（与 narrated spec 同目录）：`<stem>.script.txt`——旁白全文
逐行（供字幕工具）；`<stem>.voice.m4a`——按场景起点定位拼接的独立旁白
轨（起点 = 旁白实长 + padding 累计，与 compose 拟合公式同源）。

测试样例与冒烟：`tests/testdata/tts/`（固定旁白 wav + spec）+
`tests/unit/hf_tts_smoke.py`（本地 HTTP mock 充当 TTS 服务，不依赖
say/网络）——`ctest -R hf_tts_smoke`。

## Spec Schema

### composition（合成参数）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| id | string | `"main"` | 合成 ID（__timelines 注册键）|
| schema_version | string | `"1.0"` | spec 契约版本；与本构建支持版本不符时 compose 拒绝并提示（产物注释含 schema 锚点）|
| width / height | int | 1920 / 1080 | 画布尺寸（像素）|
| format | string | — | 画幅快捷预设：`16:9` / `9:16` / `1:1` / `4:5`，设置后覆盖 width/height |
| fps | int | 30 | 帧率 |

### background（全局背景）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| background | string | `"#000000"` | 全局背景色（CSS 颜色）|

### scenes[]（场景数组，按序播放）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| type | string | `"text"` | 场景类型：`title` / `text` / `outro` / `quote` / `list` / `stats` / `comparison` |
| text | string | `""` | 主文本 |
| subtitle | string | `""` | 副文本（仅 title 类型）|
| duration | float | 3 | 秒制时长（有 narration 时被拟合值覆盖，见下）|
| narration | string | — | 旁白音频路径（相对 spec 目录或绝对路径）；设置后场景时长 = ffprobe 音频时长 + padding |
| padding | float | 0.5 | narration 拟合时的尾部留白秒数 |
| narration_volume | float | 1.0 | 旁白混音音量（BGM 默认 0.6 衬在旁白之下）|
| transition | string | — | 场景级转场覆盖（进入方式）：`fade` / `slide-left` / `slide-right` / `wipe` / `none`；全局默认走顶层 `transitions: {type, duration}`（默认 fade / 0.5s）。语义 = 进入侧提前 duration 进入前一场景尾窗，总时长随重叠收缩。**全局 `duration` 为 0 时，场景级显式转场按 0.5s 开窗**（否则声明了转场也没有重叠窗，画面完全不动）|
| bg | string | `"#000000"` | 场景背景色 |
| text_color | string | `"#ffffff"` | 文字颜色 |
| animation | string | `"fade_in"` | 入场动画（见下表）；`none` = 不淡入，硬切进入（切割接缝用）。该场景若同时声明了 `transition`，入场回落到默认淡入——否则进入侧瞬间盖满，前一场景的出场动画全被遮住 |
| content[] | array | `[]` | 子项（text/list 类型，每项 `{text}`）|
| video | string | — | 实拍视频素材路径（相对 spec 目录或绝对路径）；生成 `<video class="scene-video" data-start/data-duration>`，画面由导出侧 FFmpeg 原生视频层供给，`visibility:hidden` 不进光栅（CEF OSR 的 `<video>` 不随 seek 重绘）。素材**自带音轨默认保留**并进导出混音与 Studio 预览混音（`muted` 只作用于页面元素，声音走音频管线）。素材导入见下方「素材自动导入」|
| video_from | number | `0` | 源视频内起点（秒）：该场景从素材的第 `video_from` 秒开始取画面 / 音轨，写进 `<video data-media-start>`（与 `data-start` 同单位：秒）。片段切割的产物——后段 = 刀口偏移，0 时完全不发该属性（未切割的 spec 产物逐字节不变）|
| captions | array | — | **场景相对秒**的分段字幕 `[{start, end, text}]`；优先于顶层 `captions.segments`。有分段时不再渲染「旁白整段字幕」|
| sfx | string | — | 场景级音效覆盖（覆盖关键词与类型默认）|

### captions（分段字幕，全局时间轴）

顶层 `captions` 是**全局绝对秒**（从合成 0 开始），由 `hf_asr.py` 的
`captions.json` 或 SRT/VTT 直接填入；`start`/`end` 落在哪个场景窗口，字幕
就自动挂到那个场景并转成场景相对时间。

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| enabled | bool | `true` | 关闭后所有字幕（含整段旁白字幕）都不渲染 |
| segments[] | array | `[]` | `[{start, end, text}]`，绝对秒；确定性排序、去重、裁剪到场景窗口，上限 800 段 |
| segments[].start / end | float | 必填 | 秒制时间窗；`end > start` 才有效 |
| segments[].text | string | 必填 | 单行字幕文本（HTML 转义，最长 300 字符）|

**渲染契约**：每个分段生成一个 `<div id="cap-{scene}-{i}" class="scene-caption seg">`
（CSS 初始 `opacity:0`），GSAP 时间线上挂三条零时长 `tl.set()`：0 秒隐藏、
段起点点亮、段终点熄灭。用 `set` 而不是带 duration 的 tween，是为了让
`tl.seek(t)` 在**正向和反向**任意时间点都精确还原状态（1ms tween 会在恰好
落在边界的帧上错一帧）。因此 `--clock seek` 逐帧导出与 Studio 拖动 scrubber
拿到的字幕完全一致，不依赖 rAF 或 CSS 动画。

各场景类型的专有字段：

| 类型 | 专有字段 |
|---|---|
| title | `subtitle` |
| text | `content[]` |
| quote | `quote`（缺省用 `text`）、`author` |
| list | `items[]`（字符串数组，或用 `content[]`）|
| stats | `value`（大数字）、`description` |
| comparison | `left` / `right`（列标题）、`left_items[]` / `right_items[]` |

旁白驱动时长拟合：`narration` 优先于 `duration`——场景 clip 窗口与 GSAP 入场游标**同源拟合**，逐场景对齐旁白长度（AI-auto-generate-video 模式）。

## 自定义模板协议

场景类型可扩展：在 `templates/`（内置）或 `<spec 目录>/templates/`（用户覆盖，优先）放 `<name>.json` 即注册一个场景类型：

```json
{
  "name": "lower-third",
  "description": "底部人名条",
  "slots": ["title", "subtitle"],
  "css": "#tpl-lower-third { ... }",
  "body": "<div class=\\"lt-title\\">{{title}}</div><div class=\\"lt-sub\\">{{subtitle}}</div>",
  "sfx": "whoosh"
}
```

- 场景 `"type": "lower-third"` 即命中注册表；`{{slot}}` 由场景同名字段替换（esc 转义、缺失置空），模板 CSS 注入 style 块，`sfx` 为该类型默认音效
- 注册表每次 compose 扫描（新模板**热加载**）；`hf_validate` 照常把守；内置示例 `templates/lower-third.json`

### animation（入场动画预设）

| 名称 | 效果 |
|---|---|
| `fade_in` | 淡入 |
| `fade_in_up` | 淡入 + 上移 |
| `fade_in_down` | 淡入 + 下移 |
| `slide_in_left` | 左滑入 |
| `slide_in_right` | 右滑入 |
| `scale_in` | 缩放入 |
| `none` | 不做入场动画（硬切进入；切割接缝默认用它） |

> 转场（`scenes[].transition`）与入场动画都会被导出侧**照实渲染到视频
> 画面上**：采集端逐帧取元素的有效 `opacity`（自身 × 祖先）与祖先
> `clip-path(inset)` 的交集，视频原生层据此设层 alpha、目标矩形与源内
> 取样矩形。所以 `fade` 是真的淡、`slide-*` 是真的位移出画、`wipe` 是真的
> 擦除，而不是"页面淡了、视频画面不动"。

### audio（背景音乐/音效）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| src | string | 必填 | 音频文件路径（相对 spec 目录或绝对路径）|
| volume | float | 0.6 | 音量（1 = 0dB，0 = 静音）|
| fade_in | float | 0 | 起点淡入秒数 |
| fade_out | float | 0 | 终点淡出秒数 |

## Agent 集成

AI agent（Claude / Cursor / Codex 等）的使用方式与 brag 一致——agent 不直接写 HTML，而是**按 schema 填 spec JSON**：

```
User: "做一个 10 秒的产品宣传片，深色背景，3 个场景"
Agent: 读本文档 schema → 填 spec JSON → python3 tools/hf_tts.py（旁白）
       → python3 tools/hf_compose.py → python3 tools/hf_validate.py
       （0 errors 才继续）→ html_bake 渲染 → 返回 MP4 路径
```

生成器保证契约合规：data-\* 秒制 + `__timelines` 注册 + GSAP 4 参数形 + 内联 GSAP（离线确定性，不依赖 CDN）。`hf_validate.py` 是 agent 的自查工具——0 errors 才交给 html_bake。

## Agent 调用协议与来源验证

**调用协议**（agent 按序执行，缺一步不算完成）：

1. 读本文档 schema → 填 spec JSON（`demo.json` 示例见上）
2. `python3 tools/hf_tts.py spec.json`（旁白，可选）
3. `python3 tools/hf_compose.py spec.narrated.json -o out.html`
4. `python3 tools/hf_validate.py out.html`——**0 errors 是硬门**
5. `html_bake --url ./out.html --out out.mp4 --seconds <总时长> --fps 30 --width W --height H --clock seek`
6. 交付产物三件套：`out.mp4` + `out.html` + `out.html_elements.json`

**约定**：agent 不手写 HTML 冒充管线产物——手写页（进阶场景）也必须过
`hf_validate`，且没有生成器注释（来源不可证，见下）。

**来源验证**（查产物、不采信 agent 口头声明）——三重锚点逐级可验：

| 锚点 | 位置 | 验证命令 |
|---|---|---|
| spec 哈希 | `out.html` 头部生成器注释 | `head -2 out.html` |
| HTML 哈希 | `out.mp4` 容器 metadata comment | `ffprobe -show_format out.mp4 \| grep comment` |
| 工具指纹 | `out.html_elements.json` generator 字段 | `grep generator out.html_elements.json` |

链路：MP4 metadata（`xmedia-kit html_bake | html sha256:…`）→ HTML 头部
（`xmedia-kit hf_compose | scenes=N | spec sha256:…`，spec 哈希 =
canonical JSON 的 sha256）→ spec 源文件。产物任一锚点缺失 = 不是本管线
生成（注意：`--chunks N>1` 的拼接产物不保留 MP4 metadata——用 HTML 注释
与 sidecar 锚点验证）。

## AI 能力集成（ASR / 导入 / Agent / MCP）

对位 Remotion 官网 `Make videos agentically` 演示里那条指令：
`Please import the videos I just recorded from ~/Downloads and put them on a
timeline, caption it.` → `add our endcard to it`。xmedia-kit 把这句话拆成
四个可独立调用的能力，全部落在同一份 spec JSON 上。

### ASR 分段字幕

`tools/hf_asr.py` 把已录素材的语音转成带时间戳的字幕段：

```bash
xmedia asr ~/Downloads/clip.mp4 -o captions.json --provider whisper
xmedia asr clip.srt -o captions.json                 # 直接读 SRT/VTT
```

| provider | 依赖 | 说明 |
|---|---|---|
| `srt` | 无 | 解析 SRT / WebVTT（`-->` 时间戳，去 HTML 标签），永远可用 |
| `whisper` | whisper.cpp | `XMEDIA_WHISPER_BIN`（默认找 `whisper-cli`）+ `XMEDIA_WHISPER_MODEL` 模型路径；ffmpeg 先抽 16k 单声道 WAV |
| `fw` | `pip install faster-whisper` | Python 包（CTranslate2，纯 CPU int8 可跑）；`--model` 选尺寸，默认 `small` |
| `auto` | — | 有字幕文件走 srt，否则 whisper → fw；两个都失败时把**各自的原因**一起报出来 |

**本机装一次就能用（已验证）**：

```bash
pip install faster-whisper "av<19"        # av>=19 去掉了 metadata_errors，fw 会解码失败
```

- 模型缓存：`~/.cache/xmedia-asr/models/<尺寸>/`（`XMEDIA_ASR_MODEL_DIR` 可改），
  首次运行自动下载；默认 `small`（中文口播明显好于 base），`XMEDIA_ASR_MODEL`
  可换成 `tiny/base/medium/large-v3` 或本地模型目录（免联网）。
- 国内网络：未设 `HF_ENDPOINT` 时，首次加载失败会**自动改用 `hf-mirror.com`
  重试一次**；镜像断流时也可以直接用 curl 断点续传把
  `config.json / model.bin / tokenizer.json / vocabulary.txt` 下到
  `~/.cache/xmedia-asr/models/small/`，之后完全离线可用。
- 速度参考（8 核 CPU / small / int8）：20s 音频约 17s，118s 素材约 1.5~2min；
  `cpu_threads` 自动取 `核数-1`。

输出统一为 `{"source", "provider", "duration", "segments":[{start,end,text}]}`，
可直接作为 spec 顶层 `captions.segments`（全局绝对秒）。`transcribe_with_fallback()`
会在 ASR 依赖缺失时自动找同目录的同名 `.srt/.vtt`（或目录里唯一一份字幕）
降级，因此**零依赖环境也能跑通「导入并加字幕」**，结果的
`fallback_from` 字段会说明降级原因。

### 素材自动导入

`tools/hf_import.py` 扫一个目录（或直接给**单个音视频文件**），用 ffprobe 读
每个视频的实长、按文件名确定性排序排到时间轴：

```bash
xmedia import ~/Downloads -o my_video.json --asr --endcard --format 9:16
xmedia import ~/Movies/kpl.mp4 -o one_clip.json     # 单文件 = 一段场景
```

- 每段视频 → 一个 `scenes[]`：`{type:"text", video:<路径>, duration:<实长>}`；
  路径相对输出 spec 所在目录，跨目录自动改绝对路径。单文件导入时 spec 目录
  默认取该文件所在目录。
- 单个文件里若不是音视频扩展名（`.srt` / `.txt` 等）直接报「不支持该文件类型」。
- `--asr` 逐素材转写，字幕段按累计场景起点偏移到全局时间轴；
  `--captions cap.srt` 直接挂外部字幕；两者可叠加。
- `--endcard` 追加内置 `outro-brand` 品牌片尾卡模板（`templates/outro-brand.json`）。
- 导入结果默认 `transitions: {"type":"none"}`（场景起点 = 时长累加，保证全局
  字幕时间戳与画面对齐）与 `sfx.enabled=false`（实拍素材不叠合成音效）。
- 目录里独立的音频文件自动作为 BGM（`audio.src`，音量 0.35）。

产物是普通 spec JSON，之后走与手写 spec **完全相同**的
TTS → compose → validate → Studio → bake 链路，三重来源锚点照常可验。

#### Studio 里的导入入口

顶栏 **导入素材** 按钮（或 Ask AI 面板的「导入素材」快捷按钮）打开导入面板；
两种方式都汇合到 `POST /api/import`，成功后自动刷新预览、时间轴与属性面板：

| 方式 | 端点 | 适用场景 |
|---|---|---|
| 本机目录 · 零拷贝 | `POST /api/import {"dir":"~/Downloads/clips"}` | 大素材：Studio 服务本机直读，不复制文件 |
| 本机单文件 | `POST /api/import {"dir":"~/Movies/kpl.mp4"}` | 只要一条素材：等价于「只含一个素材的目录」，`vid` 直接指向原文件 |
| 选择 / 拖入文件 | `POST /api/import/upload`（multipart）→ 用返回的 `dir` 再调 `/api/import` | 没有路径、只想用文件选择器；文件落到 spec 目录 `imported/<批次>/`，合计 ≤ 512 MB |

面板可设画幅（16:9 / 9:16 / 1:1 / 4:5）、ASR 自动字幕、外部
SRT/VTT/captions.json、追加片尾卡、内置音效；导入 notes 逐条显示在状态区。

合并策略：**默认替换**——时间轴只留本次导入的素材，上一次导入的画面 / 字幕 /
BGM 不会残留到新素材前面；勾选「追加到当前时间轴」（API `append: true`，兼容旧
参数 `replace: false`）才接在末尾，同一素材已在时间轴上时附一条提示。
替换是破坏性的，所以服务端会保留一份导入前的 spec 快照，`POST /api/import/undo`
（面板「撤销导入」按钮）可一键回滚。只读预览（启动时未带 `--spec`）时按钮禁用。
`GET /api/state` 的 `import.last_dir / spec_dir / upload_max_mb / can_undo /
undo_label` 供面板预填与提示。

跨目录素材在 spec 里写绝对路径。`hf_studio.py` 把 spec 显式引用过的文件登记为
只读白名单，预览页改写为 `/__xmedia_media__/<绝对路径>`，因此 HTTP 预览不会
404，而契约文件仍保留原始绝对路径（`file://` 导出语义不变）。未登记的路径
一律拒绝访问。

### Ask AI 与 Agent 规则引擎

Studio 右下角 `Ask AI`（Ctrl/⌘ + K）打开聊天面板，发一句话 →
`POST /api/agent` → 服务端 `tools/hf_agent.py` → 写回 spec → 重组 →
`refresh()` 让预览、时间轴、属性面板同步更新。快捷按钮覆盖片尾卡、竖屏、
字幕开关、旁白、ASR、导入素材、导出。

**两种后端**（面板顶栏 `→ agent` / `→ 规则引擎` 切换）：

- **规则引擎**（默认）：`POST /api/agent`，预置指令即时生效。
- **本地 agent**：`POST /api/agent/chat`，子进程驱动本机 agent CLI
  （探测顺序 claude -p → codex exec；claude 用 `--output-format json`
  取 `result` 字段，`is_error` 上报）。LLM 自主理解任意表述并多步执行
  （读 schema 文档 → 改 spec → 重跑 compose），spec 被改时服务端重载
  并重组。对话历史持久化到 `<spec 目录>/chat.jsonl`（最近 5 轮作为
  上下文注入下一次请求）。CLI 未安装或模型不可用时错误如实呈现
  （含 `claude --model` 指引），不影响规则引擎模式。
  **未命中自动转发**：规则引擎不认识的话（"我不确定…"）在 agent CLI
  可用时**自动转给 agent 处理**——用户不需要区分"哪句话该走哪条路"
  （HF chat 主界面形态）。引导类回复（缺参数/选模板）不转发。
  实测："给第一镜加一个倒计时的感觉" → CodeBuddy 理解为 3·2·1 倒计时
  标题并遵守叙事纪律（breather 节拍/两色）。
  **任意 CLI 接入**：设 `XMEDIA_AGENT_CMD`（最高优先，覆盖内置探测）
  ——JSON 数组或 shell 串，约定 = prompt 作为最后一个参数、输出纯文本
  回复；`XMEDIA_AGENT_NAME` 可选显示名。例：`export XMEDIA_AGENT_CMD=
  '["codebuddy","-p"]'`；或 claude 走 DeepSeek：`ANTHROPIC_BASE_URL=
  https://api.deepseek.com/anthropic` + `ANTHROPIC_AUTH_TOKEN=<key>`。
  **CodeBuddy 实测**（2026-11-19，全链跑通）：`npm install -g @tencent-ai/
  codebuddy-code`（Node ≥18）装好后先在终端跑一次 `codebuddy` 完成登录
  （交互模式弹 OAuth），之后
  `XMEDIA_AGENT_CMD='["codebuddy","-p","--permission-mode","acceptEdits"]'`
  接入——**--permission-mode 必需**：默认权限会拦文件写入与命令执行
  （agent 诚实报告而非假装完成——好行为）。两档实测：
  `acceptEdits` = 只放行文件编辑（够改 spec，但 agent 跑 gate/compose
  的 Bash 仍被拦）；`bypassPermissions` = 全放行（本仓库编排的 agent
  子进程跑在信任的本地上下文，推荐）。**全链实测（2026-11-19）**：
  prompt 注入 skills/launch/SKILL.md 工作流后，CodeBuddy 自主完成
  "改标题 → 重生成 contract.html → 跑 skills/launch/scripts/gate.py
  → 报告 gate PASS（9 项）"，且回复明确遵守 spec.storyboard 方向块
  纪律（"第 1 帧仍为安静节拍"）。服务进程需能找到 CLI（nvm bin 在
  PATH 里）。skills/launch/ = 对齐 HF product-launch-video 的本地
  工作流（brief → capture → design → storyboard → tts → build →
  gate → look → reply），方法论出处见 skills/STANDARDS.md。

**Pending edits 批量队列**：输入区「+ 暂存」把多次编辑攒进队列
（输入区上方暂存条：逐条展示/移除/清空 + 计数徽标），「批量应用 N」
逐条顺序执行（任一条失败即停、失败条放回队首保留余项），全部结束
统一重组一次——预览/时间轴/属性面板只刷一次。

**项目模型与每轮撤销**：spec 每次变更前快照压栈（持久化
`<spec 目录>/project.json`，上限 20 轮：{ts, message, backend,
spec_before}）；Ask AI 的 spec 变更轮回复气泡带「↩ 撤销最近一轮」
按钮——恢复该轮之前的状态并丢弃其后所有轮（时间线一致性），
`POST /api/project/undo`；`/api/state` 携带 runs 列表。

**导出面板**：顶栏「导出 MP4」打开 Export 面板——画质（540p/720p/
1080p/项目尺寸，按画幅比例等比缩放、偶数对齐）× 格式（MP4/WebM/MOV，
html_bake 按扩展名定封装）× 帧率（24/30/60）；后台子进程渲染 +
进度条 + 取消渲染（terminate + `cancelled` 标记，前端区分取消与失败）；
产物命名对齐 HF：`<项目> <YYYY-MM-DD HH.MM>.<ext>`，保存在 spec 目录。

`hf_agent.handle()` 两级设计：

1. **本地规则引擎**（零配置、确定性）：`导入 <目录|文件>` / `加片尾卡` /
   `删除第 N 个场景` / `第 N 个场景文字改成 X` / `改成竖屏` /
   `背景改成 #1a1a2e` / `关闭字幕` / `第 N 个场景时长 X 秒` /
   `生成旁白` / `自动字幕` / `导出 MP4` / `重组合成`。
   路径抽取优先引号包裹（可含空格 / 中文）→ `~/…` → `./`、`../` → 绝对路径 →
   含 `/` 的相对路径 → 带音视频扩展名的裸文件名（`kpl.mp4`）；抽不到路径时
   回引导文案，不抛「目录不存在」。
2. **可选 LLM**：环境变量 `XMEDIA_LLM_ENDPOINT`（OpenAI 兼容
   `/chat/completions`）、`XMEDIA_LLM_API_KEY`、`XMEDIA_LLM_MODEL`。
   规则未命中时把「指令 + 当前 spec」发给模型，要求只回
   `{"reply","spec"}` JSON。**密钥只在服务端**，不下发浏览器。

`hf_agent.validate_spec()` 是所有外部 spec 的硬门：结构、schema 版本、
场景类型（7 类 + 自定义模板名）、时长范围、禁止远程/脚本 URL、规模上限。
校验不过直接拒绝写回——延续「agent 不手写 HTML、按 schema 填 spec JSON」
的约定。`/api/state` 的 `agent.llm` 会在面板上显示当前是「本地规则引擎」
还是「LLM + 本地规则」。

### MCP server

`tools/xmedia_mcp.py` 是 stdio JSON-RPC 2.0 server（protocolVersion
`2024-11-05`），把编辑能力暴露给 Claude / Cursor / Codex / WorkBuddy：

```json
{
  "mcpServers": {
    "xmedia-kit": {
      "command": "python3",
      "args": ["/abs/path/to/xmedia-kit/tools/xmedia_mcp.py",
               "--studio-url", "http://127.0.0.1:8120"]
    }
  }
}
```

11 个 tools：`xmedia_state` / `recompose` / `tts` / `bake` / `job` /
`agent` / `import` / `asr` / `compose` / `validate` / `help`。其中
`import` / `asr` / `compose` / `validate` / `help` 直接调本地模块，
**Studio 服务没启动也能用**；其余通过 `--studio-url` 走 HTTP。
项目根的 `mcp.json.example` 可直接复制进 MCP 客户端配置。
stdout 严格只输出 JSON-RPC 行，日志走 stderr。

### HTTP 端点（Agent 可直接调）

| 方法 | 路径 | 作用 |
|---|---|---|
| GET | `/api/state` | spec / 场景 / 时长 / 校验结果 / `agent` 能力标记 |
| POST | `/api/recompose` | 完整 spec → 写回 + 重组预览 |
| POST | `/api/tts` | 全场景生成旁白 |
| POST | `/api/bake` · GET `/api/job?id=` | 启动导出 / 查进度 |
| POST | `/api/asr` | `{path, provider?, srt?, apply?}` → 字幕段；`apply` 直接写入 spec |
| POST | `/api/import` | `{dir, asr?, captions?, endcard?, format?}` → 生成 spec 并重组 |
| POST | `/api/import/undo` | 回滚到上一次导入前的 spec 快照 |
| GET | `/api/thumbs?path=&n=&w=&h=&s=&d=` | 片段缩略图：按 `s`/`d` 给定的源内窗口均匀抽帧（切割后各段各取各的区间），按窗口哈希缓存 |
| POST | `/api/agent` | `{message}` → 规则/LLM → spec 变更或管线动作 → 自动重组 |

## 与 brag 的对应

| brag | xmedia-kit |
|---|---|
| agent 读项目代码 | agent 读本文档 schema |
| agent 写 HyperFrames HTML | agent 填 spec JSON → hf_compose.py 生成 |
| `npx hyperframes render` | `html_bake --url file://... --out out.mp4` |
| HyperFrames runtime 管理合成 | 注入 runtime + 注册表 seek（同款契约）|

区别：brag 的 LLM 直接写 HTML（灵活但可能格式出错）；xmedia-kit 的 spec→HTML 是**确定性模板引擎**（同 spec 必出同 HTML，schema 校验挡住格式错误）——更适合无人值守的 agent 流水线。

## 嵌套合成与变量（进阶）

生成的 HTML 兼容原版（heygen-com/hyperframes）嵌套/变量语义——在 spec 的 `content` 里手写 `data-composition-src` 即可嵌套子合成：

```json
{"type": "text", "text": "", "duration": 5,
 "content": [{"text": "<div data-composition-src='./sub.html' data-variable-values='{\"title\":\"Pro\"}'></div>"}]}
```

子合成内通过 `window.__hyperframes?.getVariables?.()` 消费变量。

## 文档站与作品轮播

`python3 tools/xmedia.py docs` 从本文档生成 `docs-site/index.html`：上半页是
`tools/docs_landing.html` 模板渲染的产品介绍（Hero → 大演示窗口 → 三段
可切换介绍轮播 → 作品轮播 → 能力网格 → 实测统计），下半页仍是本文档逐字
转换的正文，附自动生成的目录与滚动高亮。纯静态、零外部依赖。

- 介绍轮播：7 秒自动推进、悬停暂停、进度条、左右按钮 / ← → 键切换；
  演示窗口左侧播放作品预览，右侧展示该作品真实 spec 节选。
- 作品轮播：横向 scroll-snap + 拖拽 / 箭头 / 分页点，视频仅在进入视口时
  懒加载与播放，离开即暂停；`prefers-reduced-motion` 下关闭自动轮播。
- 预览素材：导出产物是 MP4 容器里的 mjpeg（浏览器不保证可播），因此
  `docs` 会用 ffmpeg 转一份 H.264 预览与封面帧到 `docs-site/media/`，按源文件
  mtime 缓存；无 ffmpeg 时静默降级为占位封面，文档生成不受影响。
- 作品标题 / 描述 / 标签写在 `tools/xmedia.py` 的 `WORKS_INFO`，画幅、时长、
  fps、场景数与统计数字一律从 spec 和 ffprobe 实测，避免文档漂移。

## 已知边界

- `.clip` 生命周期：注入 runtime 接管 `.clip` 元素显隐（其揭示协议需官方管线握手）。无 `.clip` 标记的数据驱动页不受影响——**hf_compose.py 生成的 HTML 不使用 `.clip` 标记**（场景可见性由 data-start/duration + GSAP opacity 驱动）✓
- 时间单位：HF 页 `data-start/duration` 是秒，采集端自动归一到毫秒（根元素 `data-time-unit="s"` 声明，`__timelines` 注册表检测兜底）；消费端（合成/混音/元数据）统一毫秒，无需 CLI 单位开关。
- 音频轨：旁白 `<audio>` + BGM 进 html_bake 确定性混音（解码 → 修剪/增益/斜坡 → data-start 叠加，AudioConverter 统一重采样）✓。音频文件缺失时打印 `missing — skipped`、输出**无音轨**（不建零包空轨）——文件放对位置或删掉 spec 的 `audio` 段即可。
- 分块音频：`--chunks N>1` 时音频按块独立 AAC 编码再拼接，块边界存在 ±1 AAC 帧（≈21ms）的相移——内容经段级比对一致；采样级精确请用 `--chunks 1`。
- 导出画布：`--width/--height` 必须与页面 CSS 尺寸一致（hf_compose 按 format 预设生成同尺寸页面）——不一致时元素 UV 窗口错位。输出分辨率固定 = CLI 尺寸，不随显示器 DPI 变化。
- CLI 健壮性：`--url` 支持相对路径（自动补全 `file://` 绝对 URL）；未知参数显式告警（拼写错如 `--hight` 不再静默吞掉）；二进制带 `__RESTRICT` 段——外部 `DYLD_LIBRARY_PATH` 等环境变量不影响其 FFmpeg 加载（机器上共存同名旧库也安全）。
- ASR 真语音识别需要外部 provider（whisper.cpp 或 faster-whisper）；仓库零强制依赖，缺失时自动降级到 SRT/VTT 字幕文件，不会把静音素材凭空变成文字。分段字幕是逐段整行显示，暂不做逐词卡拉 OK 高亮。
- Studio 的 `Ask AI` 有两种后端（顶栏切换）：**规则引擎**（默认，零配置，覆盖常见编辑指令）与**本地 agent**（子进程驱动 `claude -p` / `codex exec`，需本机 CLI 可用，历史持久化 `<spec 目录>/chat.jsonl`）。未配置 CLI 与 `XMEDIA_LLM_ENDPOINT` 时仅规则引擎可用，agent 模式给出接入指引——不影响其余功能。
