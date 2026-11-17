import { $ } from "../shared/dom.js";
import { followPlayhead, updatePlayheadMeta } from "./driver.js";
import { phX } from "../shared/state.js";

/* prefers-reduced-motion：减少动效的用户拿到"主线程兜底推进"形态——
 * 合成层 transition 被跳过，phFrame 逐帧写 transform（I-1 允许该形态：
 * rAF 时钟本来就是这一形态的推进者）。装饰性动效由 base.css 的全局
 * media query 统一压到近零。 */
var REDUCED_MOTION = typeof window.matchMedia === "function" &&
    window.matchMedia("(prefers-reduced-motion: reduce)").matches;

export function startPhAnim(fromT) {
  var ph = $("playhead");
  if (!ph) return;
  var total = state.duration || 0;
  if (REDUCED_MOTION) {
    ph.style.transition = "none";
    ph.style.transform = "translate3d(" + phX(fromT) + "px,0,0)";
    phAnim = true;
    return;
  }
  var dur = Math.max(0.05, (total - fromT) / playRate);
  ph.style.transition = "none";
  ph.style.transform = "translate3d(" + phX(fromT) + "px,0,0)";
  void ph.offsetWidth;  // 提交起点，避免与上一次过渡串联
  ph.style.transition = "transform " + dur + "s linear";
  ph.style.transform = "translate3d(" + phX(total) + "px,0,0)";
  phAnim = true;
}
export function stopPhAnim(atT) {
  var ph = $("playhead");
  phAnim = false;
  if (!ph) return;
  ph.style.transition = "none";
  ph.style.transform = "translate3d(" + phX(atT) + "px,0,0)";
}
export function setPhTransform(t) {
  var ph = $("playhead");
  if (ph) ph.style.transform = "translate3d(" + phX(t) + "px,0,0)";
}
export function phFrame(now) {
  var dt = phLast ? Math.min(0.1, Math.max(0, (now - phLast) / 1000)) : 0;
  phLast = now;
  var total = state.duration || 0;
  if (playing) {
    phVisual = Math.min(total, phVisual + dt * playRate);
    // 播放中默认绝不写 transform：合成层动画独立推进。文本与自动滚动
    // 降频，尽量少碰主线程，也不会打断合成层插值。
    // prefers-reduced-motion 例外：合成层 transition 已被压到近零，
    // 由本帧循环逐帧写 transform 兜底推进（主线程形态，仅该用户群体）。
    if (REDUCED_MOTION) {
      setPhTransform(phVisual);
    } else if ((phTick++ % 4) === 0) {
      updatePlayheadMeta(phVisual, total);
      followPlayhead(phX(phVisual));
    }
  } else {
    phVisual = phTarget;
  }
  phRaf = requestAnimationFrame(phFrame);
}
