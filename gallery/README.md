# 示例库 — gallery/

每个 JSON 是一条可直接运行的 spec（`xmedia render` 全链）：

```bash
python3 tools/xmedia.py render gallery/product-launch.json --out gallery/launch.mp4
```

| 示例 | 展示能力 |
|---|---|
| product-launch.json | 16:9 launch 视频：自定义模板（lower-third 人名条）+ wipe/fade 转场 + stats |
| knowledge-cards.json | 9:16 竖屏知识卡片：list/stats + slide 转场 + 内嵌字幕 |
| event-invite.json | 9:16 活动邀请：wipe 转场 + quote + BGM（bgm.mp3 需自备，缺失则无音轨）|

TTS 旁白自动生成（say，`--no-tts` 跳过）。自定义模板见 docs/HF_COMPOSE.md
§自定义模板协议（内置 lower-third 示例在 templates/）。
