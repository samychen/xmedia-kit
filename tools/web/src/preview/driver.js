import { $ } from "../shared/dom.js";
import { setPhTransform, startPhAnim, stopPhAnim } from "./playhead.js";
import { ICON } from "../shared/icons.js";
import { HEAD_W, phX } from "../shared/state.js";
import { activeScene, fmtTime, fps } from "../shared/time.js";

/* ---------------- 预览驱动 ---------------- */
/* 首次真实用户手势（父页点击 / 按键）里解锁 iframe 内的 audio.play()，
   规避浏览器自动播放策略；同源直接调 API，加载未完成则下次手势重试。 */
export function unlockPreviewAudio() {
  if (audioUnlocked) return;
  var w = $("pv").contentWindow;
  if (!w || !w.__mcStudioAPI) return;
  audioUnlocked = true;
  pvCmd("enableAudio");
}
export var audioBtnState = "";
export function renderAudioBtn() {
  // 状态上报每帧一次，避免反复重建 SVG / 写 DOM。
  var key = (audioOn ? "1" : "0") + (audioBlocked ? "1" : "0");
  if (key === audioBtnState) return;
  audioBtnState = key;
  var b = $("btn-audio");
  b.innerHTML = audioOn ? ICON.soundOn : ICON.soundOff;
  b.classList.toggle("on", audioOn);
  b.title = audioOn ? "静音预览 (M)" : "开启声音 (M)";
  $("audio-hint").textContent = (audioBlocked && audioOn)
      ? "自动播放被拦截 · 点此开启声音" : "";
}
export function pvCmd(cmd, value) {
  // 显式定位立即重建合成层过渡，不等状态回传（否则跳转后游标会滞后）。
  if (cmd === "goto") {
    phVisual = phTarget = Number(value) || 0;
    if (playing) startPhAnim(phVisual); else setPhTransform(phVisual);
  } else if (cmd === "step") {
    phVisual = phTarget = Math.max(0,
        lastTime + (Number(value) || 0) / fps());
    stopPhAnim(phVisual);   // iframe 的 step 会先暂停
  } else if (cmd === "rate") {
    playRate = Math.max(0.1, Math.min(4, Number(value) || 1));
    if (playing && phAnim) startPhAnim(phVisual);
  }
  var w = $("pv").contentWindow;
  if (!w) return;
  if (!audioUnlocked && (cmd === "play" || cmd === "toggle")) {
    unlockPreviewAudio();
  }
  if (w.__mcStudioAPI) {
    var a = w.__mcStudioAPI;
    if (cmd === "play") a.play();
    else if (cmd === "pause") a.pause();
    else if (cmd === "toggle") a.toggle();
    else if (cmd === "step") a.step(value);
    else if (cmd === "goto") a.seek(value);
    else if (cmd === "loop") a.setLoop(value);
    else if (cmd === "rate") a.setRate(value);
    else if (cmd === "setMuted") a.setMuted(value);
    else if (cmd === "setVolume") a.setVolume(value);
    else if (cmd === "enableAudio") a.enableAudio();
  } else {
    w.postMessage({ mcStudioCommand: cmd, mcStudioValue: value }, location.origin);
  }
}
export function seekTo(sec) {
  phVisual = phTarget = sec;   // 时间轴点选立即对齐
  if (playing) startPhAnim(sec); else setPhTransform(sec);
  var w = $("pv").contentWindow;
  if (!w) return;
  if (w.__mcStudioAPI) w.__mcStudioAPI.seek(sec);
  else w.postMessage({ mcStudioSeek: sec }, location.origin);  /* 兼容旧服务进程 */
}
export function syncTransport(st) {
  var wasPlaying = playing;
  playing = !!st.playing;
  if (playing !== wasPlaying || !$("btn-play").innerHTML) {
    $("btn-play").innerHTML = playing ? ICON.pause : ICON.play;
    $("btn-play").title = playing ? "暂停 (Space)" : "播放 (Space)";
    $("tl-play").textContent = playing ? "‖" : "▶";
  }
  var frames = Math.max(1, st.frames || Math.round((state.duration || 0) * fps()));
  var frame = Math.min(frames, Math.max(0, st.frame || 0));
  if (+$("scrub").max !== frames) $("scrub").max = frames;
  if (+$("scrub").value !== frame) $("scrub").value = frame;
  var tcHtml = fmtTime(st.time) + " <span class=\"dim\">/ " +
      fmtTime(st.total || state.duration) + " · 帧 " + frame + "/" +
      frames + " · " + fps() + "fps</span>";
  if (tcHtml !== lastTcHtml) {
    $("tc").innerHTML = tcHtml;
    lastTcHtml = tcHtml;
  }
  if (typeof st.loop === "boolean") {
    loopOn = st.loop;
    $("btn-loop").classList.toggle("on", loopOn);
  }
  if (st.audio) {
    audioOn = !!st.audio.enabled;
    audioBlocked = !!st.audio.blocked;
    renderAudioBtn();
  }
}
export function handleState(d) {
  var st = d.mcStudioState || {};
  var total = st.total || d.total || state.duration || 0;
  var t = (typeof st.time === "number") ? st.time :
          (typeof d.mcStudioTime === "number" ? d.mcStudioTime : 0);
  var f = (typeof st.frame === "number") ? st.frame :
          (typeof d.frame === "number" ? d.frame : Math.round(t * fps()));
  var frames = st.frames || d.frames || Math.round(total * fps());
  var wasPlaying = playing, prevVisual = phVisual;
  playRate = Math.max(0.1, Math.min(4, st.rate || 1));
  lastTime = t;          // 真实播放时间，供重组恢复 / 时间轴重建
  phTarget = t;
  syncTransport({ time: t, total: total, frame: f, frames: frames,
                  playing: typeof st.playing === "boolean" ?
                           st.playing : !!d.playing, loop: st.loop,
                  audio: st.audio || d.audio });
  /* 合成层动画状态机：只在播放开始 / 暂停 / 循环回卷时重建过渡。
     播放中绝不用 iframe 上报的 t 去拉回游标：iframe 的 rAF 同样会被音频
     初始化阻塞，它的 t 会落后墙钟，拿它校正反而制造一次可见卡顿。 */
  if (!playing) {
    if (phAnim) stopPhAnim(t);
    phVisual = phTarget = t;
    setPhTransform(t);
    updatePlayheadMeta(phVisual, total);
  } else if (!wasPlaying) {
    phVisual = phTarget = t;
    startPhAnim(t);
  } else if (t < prevVisual - 0.6 && t < 0.25) {
    phVisual = phTarget = t;   // loop 回到起点
    startPhAnim(t);
  }
  // 首次收到状态 = 预览驱动已就绪。此时定位画面，避开 iframe load 事件的
  // 时序竞态：契约页入场动画在 t=0 时 opacity≈0，直接显示是一块空背景。
  // 重组重载后则恢复重组前的播放位置。
  if (!pendingApplied) {
    pendingApplied = true;
    var seek = pendingSeek;
    if (seek <= 0.001 && state.scenes.length) {
      var first = state.scenes[0];
      seek = Math.min(0.6, Math.max(0.15, (first.dur || 1) * 0.45));
    }
    if (seek > 0.001) seekTo(seek);
  }
}
/* 立即定位（暂停 / 跳转 / 时间轴重建）：写 transform + 全部文本。 */
export function updatePlayhead(t, total) {
  setPhTransform(t);
  updatePlayheadMeta(t, total);
  followPlayhead(phX(t));
}
/* 播放中只更新低频文本，不碰 transform（合成层动画正在推进）。 */
export function updatePlayheadMeta(t, total) {
  var ph = $("playhead");
  var tcTxt = fmtTime(t), tc = ph && ph.querySelector(".ph-tc");
  if (tc && tc.textContent !== tcTxt) tc.textContent = tcTxt;
  var frames = Math.round(total * fps());
  var label = fmtTime(t) + " · 帧 " +
      Math.min(Math.round(t * fps()), frames) + "/" + frames;
  if (label !== lastLabel) { $("tlabel").textContent = label; lastLabel = label; }
  var tlTc = fmtTime(t);
  if (tlTc !== lastTlTc) { $("tl-tc").textContent = tlTc; lastTlTc = tlTc; }
  var s = activeScene(t), badge = s ? s.label : "—";
  if (badge !== lastBadge) {
    $("scene-badge").textContent = badge;
    lastBadge = badge;
  }
}
/* 播放时自动跟随；手动横向滚动不会被强行拉回。低频调用，避免每帧 layout。 */
export function followPlayhead(x) {
  if (!playing) return;
  var sc = $("tl-scroll");
  if (!sc) return;
  var lo = sc.scrollLeft + HEAD_W + 24;
  var hi = sc.scrollLeft + sc.clientWidth - 40;
  var target = sc.scrollLeft;
  if (x > hi) target = x - sc.clientWidth + 80;
  else if (x < lo) target = Math.max(0, x - HEAD_W - 24);
  if (Math.abs(target - sc.scrollLeft) > 1) sc.scrollLeft = target;
}
