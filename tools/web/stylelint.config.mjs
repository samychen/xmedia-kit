// M3 阶段：存量 CSS 以「只报告不阻塞」为主；阻塞项只针对会真正出错的规则。
// 621 行拆分后的 10 个 tokens/组件样式表优先级保持原序（构建按清单拼接）。
export default {
  extends: ["stylelint-config-standard"],
  defaultSeverity: "warning",
  rules: {
    "no-descending-specificity": null,
    "selector-class-pattern": null,
    "custom-property-pattern": null,
    "declaration-block-no-duplicate-properties": "error",
    "function-no-unknown": null,
    "at-rule-no-unknown": null,
    "comment-empty-line-before": null,
    "no-invalid-position-at-import-rule": null,
  },
  ignoreFiles: ["dist/**", "node_modules/**"],
};
