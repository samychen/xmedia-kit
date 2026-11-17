export function scriptTokenize(text) {
  /* 词元切分：连续 CJK 每字一枚，其余按空格。 */
  var tokens = [], re = /[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]|[^\s\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]+/g, m;
  while ((m = re.exec(text)) !== null) tokens.push(m[0]);
  return tokens.length ? tokens : [text];
}
