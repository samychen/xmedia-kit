import js from "@eslint/js";
import globals from "globals";
import tseslint from "typescript-eslint";

/* M5：分片已是 ESM。no-undef 全面启用——跨分片符号必须走 import，
 * 唯一例外是 shared/store.js 的访问器桥（32 个可变标量，经
 * globalThis accessor 保持原名读写），在 globals 里声明为 writable。 */
const BRIDGED = {
  state: "writable", selIdx: "writable", loopOn: "writable",
  playing: "writable", lastTime: "writable", curK: "writable",
  pendingSeek: "writable", pendingApplied: "writable",
  audioOn: "writable", audioBlocked: "writable", audioUnlocked: "writable",
  lastTcHtml: "writable", lastLabel: "writable", lastBadge: "writable",
  lastTlTc: "writable", phVisual: "writable", phTarget: "writable",
  phLast: "writable", phRaf: "writable", phTick: "writable",
  phAnim: "writable", playRate: "writable", tlPPS: "writable",
  tlFitPPS: "writable", tlFitted: "writable", tlSnap: "writable",
  tlRange: "writable", tlRangeMode: "writable", rangeLoop: "writable",
  curFields: "writable", askBusy: "writable", askBackend: "writable",
};

export default tseslint.config(
  { ignores: ["dist/", "node_modules/", "tests/baseline/", "**/*.min.*"] },
  js.configs.recommended,
  ...tseslint.configs.recommended.map((c) => ({
    ...c,
    files: ["src/**/*.js", "tests/unit/**/*.ts", "scripts/**/*.mjs", "tests/e2e/**/*.mjs"],
    languageOptions: {
      ...c.languageOptions,
      globals: { ...globals.browser, ...BRIDGED },
    },
    rules: {
      ...c.rules,
      "no-unused-vars": "warn",
      "@typescript-eslint/no-unused-vars": "warn",
      "@typescript-eslint/no-explicit-any": "off",
      "prefer-const": "off",
      "no-var": "off",
    },
  })),
  {
    files: ["tests/unit/*.mjs"],
    rules: { "no-undef": "off" },
  },
);
