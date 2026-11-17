export var fps = function () {
  return (state.spec && state.spec.composition &&
          state.spec.composition.fps) || 30;
};






















export function fmtTime(sec) {
  var s = Math.max(0, sec || 0), m = Math.floor(s / 60);
  var r = s - m * 60;
  var whole = Math.floor(r), cs = Math.round((r - whole) * 100);
  if (cs === 100) { whole += 1; cs = 0; }
  return (m < 10 ? "0" : "") + m + ":" + (whole < 10 ? "0" : "") + whole +
         "." + (cs < 10 ? "0" : "") + cs;
}
export function activeScene(t) {
  for (var i = 0; i < state.scenes.length; i++) {
    var s = state.scenes[i];
    if (t >= s.start && t < s.start + s.dur) return s;
  }
  return null;
}
