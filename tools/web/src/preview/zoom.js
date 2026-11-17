import { $ } from "../shared/dom.js";
import { ZOOM_STEPS } from "../shared/state.js";

/* ---------------- 预览画布缩放 ---------------- */
export function fitPreview() {
  var wrap = $("pvwrap"), fr = $("pvframe"), stage = $("stage");
  var W = state.width || 1080, H = state.height || 1920;
  fr.style.width = W + "px";
  fr.style.height = H + "px";
  $("pv").style.width = W + "px";
  $("pv").style.height = H + "px";
  var availW = Math.max(80, wrap.clientWidth - 48);
  var availH = Math.max(80, wrap.clientHeight - 48);
  var mode = $("zoom").value;
  var k = (mode === "fit") ? Math.min(availW / W, availH / H)
                           : parseFloat(mode);
  k = Math.max(0.05, Math.min(k, 4));
  curK = k;
  stage.style.width = (W * k) + "px";
  stage.style.height = (H * k) + "px";
  fr.style.transform = "scale(" + k + ")";
  $("zoom-label").textContent = Math.round(k * 100) + "%";
}
export function zoomStep(dir) {
  var mode = $("zoom").value;
  var k = mode === "fit" ? curK : parseFloat(mode);
  var idx = 0;
  for (var i = 0; i < ZOOM_STEPS.length; i++) {
    if (ZOOM_STEPS[i] <= k + 0.001) idx = i;
  }
  idx = Math.max(0, Math.min(ZOOM_STEPS.length - 1, idx + dir));
  $("zoom").value = String(ZOOM_STEPS[idx]);
  fitPreview();
}
