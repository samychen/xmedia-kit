# SFX 音效素材（零授权，ffmpeg 程序合成）

五个基础音效由 ffmpeg 程序合成——无第三方版权、无署名义务，可随时用
以下命令重新生成（在 assets/sfx/ 下执行）：

```bash
ffmpeg -y -f lavfi -i "anoisesrc=d=0.5:c=brown:a=0.8" \
  -af "lowpass=f=1200,afade=t=in:d=0.15,afade=t=out:st=0.25:d=0.25,volume=0.9" whoosh.wav
ffmpeg -y -f lavfi -i "sine=frequency=880:duration=0.09" \
  -af "afade=t=out:st=0.02:d=0.07,volume=0.8" pop.wav
ffmpeg -y -f lavfi -i "sine=frequency=1320:duration=0.5" \
  -af "afade=t=out:st=0.05:d=0.45,volume=0.7" ding.wav
ffmpeg -y -f lavfi -i "sine=frequency=660:duration=0.12" \
  -f lavfi -i "sine=frequency=990:duration=0.3" \
  -filter_complex "[0:a][1:a]concat=n=2:v=0:a=1,afade=t=out:st=0.25:d=0.15,volume=0.7" success.wav
ffmpeg -y -f lavfi -i "anoisesrc=d=0.35:c=white:a=0.6" \
  -af "highpass=f=400,lowpass=f=3000,afade=t=in:d=0.1,afade=t=out:st=0.15:d=0.2,volume=0.7" transition.wav
```

hf_compose 的三级选择：场景 `sfx` 字段覆盖 → 文本关键词匹配（见
tools/hf_compose.py 的 SFX_KEYWORDS）→ 场景类型默认；发射为
`<audio>` 元素按场景起点叠加进混音（默认音量 0.4）。关闭方式：
`--no-sfx` 或 spec `"sfx": {"enabled": false}`。
