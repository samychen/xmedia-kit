export var $ = function (id) { return document.getElementById(id); };




export function log(msg, cls) {
  var el = document.createElement("div");
  if (cls) el.className = cls;
  el.textContent = msg;
  $("log").appendChild(el);
  $("log").scrollTop = $("log").scrollHeight;
}










export function escHtml(s) {
  return String(s == null ? "" : s).replace(/&/g, "&amp;")
      .replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
}
