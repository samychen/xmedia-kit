/* Studio 全局可变状态（M5 store）。
 *
 * 跨分片写（rebind）的可变标量收进单一对象 S；分片代码经访问器桥保持
 * 原名读写（见下方 defineProperty），业务代码零改写。playback 组
 * （phVisual/phTarget/playing/lastTime 等）按 I-1 不走响应式订阅——位置
 * 推进始终由合成层动画承担，rAF 时钟只做低频文本回填。
 *
 * 初始值逐字取自迁移前 tools/studio_ui.html 的顶层 var 声明。
 */
export const S = {
  /* 文档状态（refresh() 里有整体重绑定 `state = s`，故必须走桥） */
  state: { spec: {}, duration: 0, scenes: [], audios: [],
           width: 1080, height: 1920, errors: 0, findings: [] },
  /* 场景选择 / 传输条 */
  selIdx: -1, loopOn: false, playing: false, lastTime: 0, curK: 1,
  pendingSeek: 0, pendingApplied: false,
  /* 预览声音：audioOn 静音开关、audioBlocked 浏览器自动播放拦截提示、
     audioUnlocked 首次用户手势解锁（同源 iframe 内 audio.play() 需要手势）。 */
  audioOn: true, audioBlocked: false, audioUnlocked: false,
  /* 状态上报每帧一次（60fps）：只在文本真的变化时写 DOM，避免反复重建
     innerHTML / 写滚动位置造成额外重排，让播放头移动更顺滑。 */
  lastTcHtml: "", lastLabel: "", lastBadge: "", lastTlTc: "",
  /* 播放头视觉时钟：phVisual 只用于时间码、场景标签、自动滚动这些低频
     文本更新；位置推进由合成层动画承担（I-1）。 */
  phVisual: 0, phTarget: 0, phLast: 0, phRaf: 0, phTick: 0,
  phAnim: false, playRate: 1,
  /* 时间轴：tlPPS = 每秒像素（可缩放），tlSnap = 帧/边界吸附。 */
  tlPPS: 60, tlFitPPS: 60, tlFitted: false, tlSnap: true,
  /* 区间选择（in / out）：tlRange = {a, b}（秒，a<b），tlRangeMode = 拖拽即框选，
     rangeLoop = 到 out 点回卷到 in 点继续播。NLE 惯例：选区外压暗、选区内原色。 */
  tlRange: null, tlRangeMode: false, rangeLoop: false,
  /* Inspector 当前字段集（scenes 定义、inspector 重写） */
  curFields: [],
  /* Ask AI 后端选择与忙标志（ask 定义、pending 重写） */
  askBusy: false, askBackend: "rule",
};

/* 访问器桥：globalThis 上的同名 accessor。严格模式下 `name = v` 经
 * setter 合法（不是未声明变量赋值）。store.js 必须是 main.js 的
 * 第一个 import——任何分片的顶层副作用求值前桥必须就位。 */
/* 被动订阅（M5 组件化底座）：watch(key, fn) 在对应键被写时收到通知。
 * 播放组（phVisual/phTarget/playing/lastTime 等）按 I-1 不进响应式订阅——
 * 通知是被动的：没有订阅者时零行为变化，订阅者自己决定是否回流 DOM。 */
const _watchers = new Map();
export function watch(key, fn) {
  if (!_watchers.has(key)) _watchers.set(key, new Set());
  _watchers.get(key).add(fn);
  return () => _watchers.get(key).delete(fn);
}
function _notify(key) {
  const fns = _watchers.get(key);
  if (fns) for (const fn of fns) fn(S[key]);
}
for (const k of Object.keys(S)) {
  Object.defineProperty(globalThis, k, {
    get() { return S[k]; },
    set(v) { S[k] = v; _notify(k); },
    configurable: true,
  });
}
