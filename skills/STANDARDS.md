# Writing a Studio skill（xmedia-kit 版）

如何编写、接线与检查 `skills/` 下的技能。方法论的出处：
[heygen-com/hyperframes](https://github.com/heygen-com/hyperframes) 的
`packages/desktop/skills/STANDARDS.md`（Apache 2.0）——三层模型、gate
循环与 options-not-questions 原则直接沿用；执行细节全部改为本仓库的
确定性管线（spec → 契约页 → html_bake），不是 agent 逐帧自由创作。

## 1. Skill 是干什么的

把人送到一个**愿意继续编辑的第一稿**：约 80% 的成片在编辑器里完成。
两条路径，都不是"教用户怎么做视频"：

1. **帮人说清想要什么**。人通过回应具体事物弄清自己想要什么。给出
   尖锐不同的三个方向，听人说哪 好，把它变成后续运行的约束。开头的
   问卷得到的答案不如三个好选项。
2. **保持产出多样**。同样的请求不应总产出同一支片。默认是起点，参考
   与人的偏好可以推翻，允许混搭。

Skill 三层，各居其位：

| 层 | 内容 | 可否覆盖 | 位置 |
| --- | --- | --- | --- |
| **Floor** | 每个产出必须保有的底线；主要由 gate 检查 | 只能由人、用文字 | `references/floor.md` + `scripts/gate.py` |
| **默认方向** | 有出处的、在成功作品上量过的手法 | 可以：被参考或人推翻，说一声 | `references/` 各领域文件 |
| **学习方法** | 怎么为本项目找、读、量参考 | n/a（是方法） | `references/study.md` |

## 2. 布局

```
<group>/                      一个家族一个目录（launch……）
  SOURCE.md                   出处、接线、每个文件的 sha256（§8）
  <skill>/
    SKILL.md                  运行顺序；≤250 行
    references/               步骤指向时才读；一文件一职责
    scripts/                  确定性工作；gate 在这里
```

- SKILL.md 每次运行都读，只放顺序与决策；查询类内容进 references。
- 超 300 行的 reference 以目录开头；文件名短而直（`story.md`）。

## 3. Frontmatter

```yaml
---
name: launch # 等于目录名
description: "<产出什么、从什么输入。> <用人的话说何时用。> <什么去别的技能（→ other-skill）。> <必须过的 gate。> [关键词…]"
---
```

- 300–800 字符。技能容易欠触发，慷慨列出触发场景。
- 路由写进 description：哪些消息进来、哪些去别的技能。

## 4. 正文顺序

1. **Pitch**（2–5 行）：好结果是什么 + 三层与文件。
2. **Order:** 一行写完整管线（brief → capture → design → storyboard → build → gate → look → reply）。
3. **Read before you ask**：先读素材再提问。
4. **Options**：人选择的停点（§5）。
5. **Plan 文件**：任何契约页之前写好；是运行的记忆（§6）。
6. **Build** 步骤，每步点名管辖的 reference。
7. **Gate** → **look** → **reply**（§6）。
8. **Notes**：后续轮次怎么读与改。

## 5. 帮人说清想要什么

- **先读后问**：素材能回答的不问。
- **给选项不提问**：三个方向，推荐者在前附一句理由。
- **从不问工艺**（缓动/转场/字体），除非人先提了。
- **未知不阻塞**：有标签的占位、集中一处、继续走。
- **人说的变成约束**：写进 plan 文件的 notes，后续每步遵守。

## 6. Floor 与 gate

- 每个技能自带 gate 脚本，回复前必须 exit 0。格式：逐行
  `FAIL <rule>: <what and where>`，末行 `RESULT: PASS/FAIL`，失败
  exit 非零。
- **gate 后 look**：用分镜真实帧（`/api/sb-frames` 或 `xmedia.py`
  渲染抽帧）**agent 自己看**。用数字与帧验证；"看起来顺"不是验证。
- 禁令要具体（`repeat: -1`、交叉溶解、弹跳），每条带替代做法。

## 7. 默认方向

- **每个数字有出处**：`<spec>@<秒>`、模板名、量过的笔记。无出处的
  数字删除。
- **每个默认都写何时不适用**——这句让 agent 可以有理由偏离。
- 默认不与 floor 冲突。

## 8. Plan 文件

每项目一份 JSON plan，在任何契约页之前写好并保留：它是计划、分镜的
源头、gate 的输入、新会话替代 chat 读的东西。本仓库即
`<项目目录>/spec.json` 的 `storyboard` 段（arc/message/direction/
frames）+ `project.json` 的 runs。

## 9. 移交编辑与回复

- 为编辑而建：场景命名清晰、可换件（模板场景）与锁定件（旁白音频）
  分明；回复里说明哪些是占位。
- 只写 `spec.json` 与 `contract.html`；不要起服务、不要自己导出——
  app 预览与导出。
- 用平实的话回复：结果说了什么、多长、哪些是占位、哪些默认被打破及
  为什么。**从不声称 gate 或快照没展示的东西。**
- 把感受变成量过的改动，只改被点名的（"太快" → 所有 zoom 0.7×）。

## 10. 脚本与接线

- **脚本**：Python，`python3 tools/<SKILL_DIR>/scripts/<name>.py`；
  stdlib + 仓库内模块（hf_compose/hf_validate/hf_storyboard）。
  一切可 seek 的都是时间的纯函数。
- **接线**：`/api/agent/chat` 的 prompt 在
  `spec.storyboard` 存在时注入 skill 路径（agent 读 SKILL.md 按
  Order 执行）；技能目录发现 = `skills/<group>/<skill>/SKILL.md`。
- **Try it before merging**：2–3 条真实 prompt 用新旧 skill 各跑
  一次，产物并排比。

## 11. 文风

命令式、平实、短句。规则为何存在用一句从句说；大写与"MUST"是"为什么
没写出来"的信号。数字、名字、例子代替形容词。一个事实只活在一个文件
里，其他地方指向它。
