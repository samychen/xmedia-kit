/* ---------------- 双屏同钟对照（HF "Show original"） ----------------
 * 参考视频面板贴预览左侧，与成品共用播放头：播放中偏差 > 0.25s 才
 * seek（避免逐帧 currentTime 赋值触发反复缓冲——预览声音一节同款教训）；
 * 暂停 / 拖动 / seek 时严格对齐。路径经 /api/reference/video 登记白名单
 * 后以 /__xmedia_media__/<realpath> 直读，无需上传。
 */
import { api } from "../shared/api-client.js";
import { $, log } from "../shared/dom.js";

var refOn = false, refEl = null, refTimer = 0, refLastSync = -1;

function refSync() {
  if (!refOn || !refEl) return;
  var t = phVisual;   // store 桥原名（phVisual 在 S 顶层，不在 state 上）
  if (Math.abs(refLastSync - t) < 0.04) return;   // 播放头没动就不对齐
  refLastSync = t;
  var drift = Math.abs(refEl.currentTime - t);
  if (drift > 0.25) {
    try {
      refEl.currentTime = Math.min(Math.max(0, t), refEl.duration || t);
    } catch (e) { /* metadata 未就绪时 currentTime 赋值会抛——下轮再试 */ }
  }
  if (!playing && !refEl.paused) refEl.pause();   // 桥原名（S 不在 globalThis）
}

function refStart(path) {
  path = String(path || "").trim();
  if (!path) return;
  api("/api/reference/video", { path: path }).then(function (r) {
    refEl = $("refvideo");
    refEl.src = r.url;
    $("ref-panel").classList.add("open");
    $("ref-pick").style.display = "none";
    $("btn-ref").classList.add("on");
    refOn = true;
    if (!refTimer) {
      refTimer = setInterval(refSync, 120);
    }
    log("参考对照已开启：" + r.path, "ok");
  }).catch(function (e) {
    log("对照开启失败：" + e.message, "err");
  });
}

function refClose() {
  refOn = false;
  $("ref-panel").classList.remove("open");
  $("btn-ref").classList.remove("on");
  $("ref-pick").style.display = "flex";
  $("ref-path").value = "";
  if (refEl) {
    refEl.pause();
    refEl.removeAttribute("src");
    refEl.load();
  }
}

$("btn-ref").onclick = function () {
  if (refOn) {
    refClose();
    return;
  }
  $("ref-panel").classList.add("open");
  $("ref-path").focus();
};
$("ref-go").onclick = function () { refStart($("ref-path").value); };
$("ref-path").addEventListener("keydown", function (ev) {
  if (ev.isComposing || ev.keyCode === 229) return;  // CJK 组合中不触发
  if (ev.key === "Enter") refStart($("ref-path").value);
});
$("ref-close").onclick = refClose;
