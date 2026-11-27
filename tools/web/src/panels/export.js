import { api } from "../shared/api-client.js";
import { log, $ } from "../shared/dom.js";
import { fps } from "../shared/time.js";

/* ---------------- 导出面板（对齐 HF Export） ---------------- */
export var expJob = -1, expPolling = false;
function handleBakeDone(j) {
  expSetBusy(false);
  $("exp-progress").classList.add("on");
  $("exp-bar").style.width = "100%";
  if (j.cancelled) {
    $("exp-progress-text").textContent = "已取消";
    $("exp-progress-text").classList.remove("err");
  } else if (j.rc === 0) {
    $("exp-progress-text").classList.remove("err");
    $("exp-progress-text").textContent = "导出完成：" + j.out;
    var rv = document.createElement("button");
    rv.className = "btn"; rv.style.marginLeft = "8px";
    rv.textContent = "在 Finder 中显示";
    rv.onclick = function () {
      api("/api/reveal", { path: j.out }).then(function () {
        log("已在 Finder 中定位：" + j.out, "ok");
      }).catch(function (e) {
        log("定位失败：" + e.message, "err");
      });
    };
    $("exp-progress-text").appendChild(rv);
    log("导出完成：" + j.out, "ok");
    log("验源：ffprobe -show_format \"" + j.out + "\" | grep comment", "ok");
  } else {
    $("exp-progress-text").classList.add("err");
    $("exp-progress-text").textContent = "导出失败 rc=" + j.rc +
        (j.tail ? "：" + j.tail.split("\n").pop() : "");
    log("导出失败 rc=" + j.rc + "：" + j.tail, "err");
  }
  expPolling = false;
}
function handleBakeError(e) {
  expSetBusy(false);
  expPolling = false;
  $("exp-progress").classList.add("on");
  $("exp-progress-text").classList.add("err");
  $("exp-progress-text").textContent = "导出失败：" + e.message;
  log("导出失败：" + e.message, "err");
}
export function expOpen(on) {
  /* 打开面板时恢复进行中任务的进度显示（否则任务在跑但界面看不到，
     再点渲染会撞「已有导出任务在跑」——用户感知为「无法导出」）。 */
  if (on) {
    api("/api/job/current").then(function (r) {
      if (r.job !== null && r.job !== undefined && !expPolling) {
        expJob = r.job;
        expPolling = true;
        expSetBusy(true);
        expPoll(r.job).then(handleBakeDone).catch(handleBakeError);
      }
    }).catch(function () {});
  }
  var panel = $("exp-panel");
  panel.classList.toggle("open", !!on);
  if (on) {
    if (!panel.open) panel.show();   // 非模态：保持可同时看预览的既有行为
    $("exp-dim").textContent = "项目尺寸 " + state.width + "×" +
        state.height + " · 总时长 " + (state.duration || 0).toFixed(1) +
        "s · 产物保存到 spec 所在目录";
  } else if (panel.open) {
    panel.close();
  }
}
export function expSetBusy(busy) {
  $("exp-go").disabled = busy;
  $("exp-cancel").disabled = !busy;
  $("exp-progress").classList.toggle("on", busy);
  if (!busy) $("exp-bar").style.width = "0%";
}
export function expPoll(id) {
  return api("/api/job?id=" + id).then(function (j) {
    if (!expPolling) return j;   // 用户取消后停止刷新 UI
    var total = Math.max(0.5, state.duration || 0.5);
    var frac = Math.min(0.97, j.elapsed / (total * 4));
    $("exp-bar").style.width = (frac * 100).toFixed(0) + "%";
    $("exp-progress-text").textContent = "渲染中 " +
        j.elapsed.toFixed(0) + "s（" + (j.out || "").split("/").pop() + "）";
    if (j.running) {
      return new Promise(function (res) {
        setTimeout(res, 700);
      }).then(function () { return expPoll(id); });
    }
    return j;
  });
}
$("exp-x").onclick = function () { expOpen(false); };
$("exp-cancel").onclick = function () {
  expPolling = false;
  api("/api/bake/cancel", {}).then(function () {
    log("导出已取消", "err");
  }).catch(function (e) {
    log("取消失败：" + e.message, "err");
  }).then(function () {
    expSetBusy(false);
    $("exp-progress-text").textContent = "已取消";
    $("exp-progress").classList.add("on");
  });
};
$("exp-go").onclick = function () {
  var quality = $("exp-quality").value;
  var fmt = $("exp-fmt").value;
  var fps = parseInt($("exp-fps").value, 10);
  expSetBusy(true);
  expPolling = true;
  $("exp-progress-text").classList.remove("err");
  $("exp-progress-text").textContent = "准备中…";
  log("导出开始：" + quality + " / " + fmt + " / " + fps + "fps");
  api("/api/bake", { quality: quality, fmt: fmt, fps: fps })
    .then(function (r) { expJob = r.job; return expPoll(r.job); })
    .then(handleBakeDone)
    .catch(handleBakeError);
};
export function pollJob(id) {
  return api("/api/job?id=" + id).then(function (j) {
    if (j.running) {
      $("job").textContent = "导出中 " + j.elapsed.toFixed(0) + "s";
      return pollJob(id);
    }
    return j;
  });
}
