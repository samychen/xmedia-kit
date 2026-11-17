import { $ } from "./shared/dom.js";
import { scriptRender } from "./panels/script.js";
import { pvCmd, renderAudioBtn, seekTo, unlockPreviewAudio } from "./preview/driver.js";
import { phFrame } from "./preview/playhead.js";
import { fitPreview } from "./preview/zoom.js";
import { ICON } from "./shared/icons.js";
import { fps } from "./shared/time.js";
import { clearRange, pushRange, rangeOpen, syncRangeUI } from "./timeline/range.js";
import { fitTimeline, zoomTimeline, zoomToScene } from "./timeline/render.js";
import { splitAtPlayhead } from "./timeline/trim.js";

/* ---------------- 交互绑定 ---------------- */
$("btn-play").innerHTML = ICON.play;
renderAudioBtn();
phRaf = requestAnimationFrame(phFrame);  // 播放头自持的匀速视觉时钟
$("btn-play").onclick = function () { pvCmd("toggle"); };
$("btn-first").onclick = function () { pvCmd("goto", 0); };
$("btn-last").onclick = function () {
  pvCmd("goto", Math.max(0, state.duration - 1 / fps()));
};
$("btn-prev").onclick = function () { pvCmd("step", -1); };
$("btn-next").onclick = function () { pvCmd("step", 1); };
$("btn-loop").onclick = function () {
  loopOn = !loopOn;
  $("btn-loop").classList.toggle("on", loopOn);
  pvCmd("loop", loopOn);
};
$("rate").onchange = function () { pvCmd("rate", parseFloat(this.value)); };
$("btn-audio").onclick = function () {
  audioOn = !audioOn;
  pvCmd("setMuted", !audioOn);
  renderAudioBtn();
};
$("vol").addEventListener("input", function () {
  pvCmd("setVolume", parseFloat(this.value) / 100);
});
$("audio-hint").onclick = function () {
  audioBlocked = false;
  audioUnlocked = false;
  unlockPreviewAudio();
  pvCmd("enableAudio");
  renderAudioBtn();
};
/* 任意用户手势都尝试解锁一次 iframe 内音轨（自动播放策略）。 */
document.addEventListener("pointerdown", unlockPreviewAudio, true);
document.addEventListener("keydown", unlockPreviewAudio, true);
$("scrub").addEventListener("input", function () {
  pvCmd("goto", parseFloat(this.value) / fps());
});
$("zoom").onchange = fitPreview;
$("bg-mode").onchange = function () {
  $("pvwrap").className = this.value;
  if ($("btn-grid").classList.contains("on")) $("pvwrap").classList.add("grid");
  if ($("btn-safe").classList.contains("on")) $("pvwrap").classList.add("safe");
};
$("btn-grid").onclick = function () {
  this.classList.toggle("on");
  $("pvwrap").classList.toggle("grid", this.classList.contains("on"));
};
$("btn-safe").onclick = function () {
  this.classList.toggle("on");
  $("pvwrap").classList.toggle("safe", this.classList.contains("on"));
};
$("btn-full").onclick = function () {
  if (document.fullscreenElement) { document.exitFullscreen(); return; }
  if ($("pvwrap").requestFullscreen) {
    $("pvwrap").requestFullscreen().then(fitPreview, function () {});
  }
};
/* 时间轴工具栏：与预览传输条复用同一套驱动命令。 */
$("tl-first").onclick = function () { pvCmd("goto", 0); };
$("tl-last").onclick = function () {
  pvCmd("goto", Math.max(0, state.duration - 1 / fps()));
};
$("tl-prevf").onclick = function () { pvCmd("step", -1); };
$("tl-nextf").onclick = function () { pvCmd("step", 1); };
$("tl-play").onclick = function () { pvCmd("toggle"); };
$("tl-fit").onclick = fitTimeline;
$("tl-zoom-in").onclick = function () { zoomTimeline(1.4); };
$("tl-zoom-out").onclick = function () { zoomTimeline(1 / 1.4); };
$("tl-zoom-sel").onclick = function () { zoomToScene(selIdx); };
$("tl-split").onclick = function () { splitAtPlayhead(); };
/* 区间：拖拽框选 / 两端把手 / 区间内播放与循环。 */
$("tl-range-mode").onclick = function () {
  tlRangeMode = !tlRangeMode;
  syncRangeUI();
};
$("tl-range-play").onclick = function () {
  if (!rangeOpen()) return;
  seekTo(tlRange.a);
  pushRange();
  pvCmd("play");
};
$("tl-range-loop").onclick = function () {
  rangeLoop = !rangeLoop;
  syncRangeUI();
  pushRange();
  if (rangeLoop && rangeOpen()) {
    if (lastTime < tlRange.a - 0.02 || lastTime >= tlRange.b - 0.02) {
      seekTo(tlRange.a);
    }
  }
};
$("tl-range-clear").onclick = function () { clearRange(); };
$("tl-snap").onclick = function () {
  tlSnap = !tlSnap;
  this.classList.toggle("on", tlSnap);
  this.textContent = tlSnap ? "吸附 开" : "吸附 关";
};
$("btn-log").onclick = function () {
  var el = $("log");
  el.classList.toggle("open");
  this.classList.toggle("on", el.classList.contains("open"));
};
$("tab-scenes").onclick = function () {
  $("tab-scenes").classList.add("active");
  $("tab-script").classList.remove("active");
  $("tab-source").classList.remove("active");
  $("scenes-panel").style.display = "flex";
  $("script-panel").style.display = "none";
  $("source-panel").style.display = "none";
};
$("tab-script").onclick = function () {
  $("tab-script").classList.add("active");
  $("tab-scenes").classList.remove("active");
  $("tab-source").classList.remove("active");
  $("script-panel").style.display = "flex";
  $("scenes-panel").style.display = "none";
  $("source-panel").style.display = "none";
  scriptRender();
};
$("tab-source").onclick = function () {
  $("tab-source").classList.add("active");
  $("tab-scenes").classList.remove("active");
  $("tab-script").classList.remove("active");
  $("source-panel").style.display = "flex";
  $("scenes-panel").style.display = "none";
  $("script-panel").style.display = "none";
};
