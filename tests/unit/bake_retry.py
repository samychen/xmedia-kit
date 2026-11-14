#!/usr/bin/env python3
# CEF 烘焙类冒烟的重试包装：同一会话里前一个 CEF 测试退出后，GPU/进程
# 环境偶发抖动会让新 CEF 会话的 VT 排队饿死（paint 超时）——单独运行必
# 过。失败后冷却 3s 重试一次，仍失败才如实上报。
import subprocess
import sys
import time

DELAY = 3
ATTEMPTS = 2

cmd = sys.argv[1:]
proc = None
for attempt in range(1, ATTEMPTS + 1):
    proc = subprocess.run(cmd)
    if proc.returncode == 0 or attempt == ATTEMPTS:
        break
    print(f"[bake_retry] attempt {attempt} failed rc={proc.returncode}, "
          f"cooldown {DELAY}s then retry", file=sys.stderr)
    time.sleep(DELAY)
sys.exit(proc.returncode if proc else 1)
