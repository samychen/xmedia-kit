import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const webRoot = join(dirname(fileURLToPath(import.meta.url)), "..");
const dist = readFileSync(join(webRoot, "dist", "index.html"));
const shipped = readFileSync(join(webRoot, "..", "studio_ui.html"));
if (!dist.equals(shipped)) {
  console.error(
    "STALE BUILD: tools/studio_ui.html 与 tools/web/dist/index.html 不一致。\n" +
      "修复：cd tools/web && npm run build（分片源码是唯一事实，产物必须重建）",
  );
  process.exit(1);
}
console.log("freshness ok: tools/studio_ui.html == dist/index.html (" + dist.length + " bytes)");
