import { defineConfig } from "vitest/config";
import { fileURLToPath } from "node:url";

// vitest 的相对路径解析在 tests/unit → ../src 链路上有怪癖（resolved id
// 保持相对形态导致加载失败），用别名指向 src 根，测试一律从 @web/ 导入。
const srcRoot = fileURLToPath(new URL("./src/", import.meta.url));

export default defineConfig({
  resolve: {
    alias: {
      "@web": srcRoot,
    },
  },
  test: {
    include: ["tests/unit/**/*.test.ts"],
    environment: "node",
    // 沙箱加载整包 JS，单线程足够（套件 <200ms），也避免多 worker 内存翻倍
    pool: "threads",
    poolOptions: { threads: { minThreads: 1, maxThreads: 1 } },
  },
});
