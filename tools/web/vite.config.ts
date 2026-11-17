import { defineConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";

/* Studio UI 单文件构建：dist/studio_ui.html 自包含（JS/CSS 内联、离线可用），
   由 scripts/check-freshness.mjs 校验后覆盖 tools/studio_ui.html——
   npm 发布与 hf_studio.py 的磁盘读取路径均不感知构建过程。 */
export default defineConfig({
  root: "src",
  base: "./",
  plugins: [viteSingleFile()],
  build: {
    outDir: "../dist",
    emptyOutDir: true,
    target: "es2022",
    cssCodeSplit: false,
    assetsInlineLimit: 100_000_000,
  },
});
