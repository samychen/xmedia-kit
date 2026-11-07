// CefPlatform — shared subprocess entry (both platforms).
//
// Linux single-binary mode (cefsimple convention): render-demo is both the
// browser process and the CEF subprocess. Chromium relaunches the executable
// as zygote / gpu / renderer with `--type=<role>`; render_demo main() checks
// CefPlatformIsSubprocess() as its very first action and delegates here so
// the subprocess never touches GLFW / glog / FFmpeg / the render pipeline.
//
// macOS: helpers are standalone Helper.app bundles (cef_helper_mac.mm with
// their own main), so the host binary is never relaunched with --type=; the
// implementation below is still correct if that ever happens.
//
// NOTE: glog is NOT initialized in the subprocess (main() returns before
// InitGoogleLogging) — use fprintf(stderr, ...) here, never RENDER_LOG.

#include "cef_platform.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include <cstdio>
#include <cstring>

#include "cef_app.h"

#include "include/cef_app.h"

namespace xmedia {
namespace cef {

bool CefPlatformIsSubprocess(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (arg && std::strncmp(arg, "--type=", 7) == 0) {
      return true;
    }
  }
  return false;
}

int CefPlatformSubprocessMain(int argc, char** argv) {
#if defined(CEF_USE_SANDBOX)
  // Sandbox builds must wrap CefExecuteProcess in CefScopedSandboxContext
  // (see cef_helper_mac.mm). We build with no_sandbox=true and without
  // libcef_sandbox, so this branch is unreachable in our configuration.
  std::fprintf(stderr, "[cef] subprocess: sandbox build not supported here\n");
  return 1;
#else
  CefMainArgs main_args(argc, argv);
  CefRefPtr<MediaCefApp> app(new MediaCefApp);
  // Returns >0 only when the process should exit (subprocess roles); the
  // browser process would have --type= absent and never reaches this call.
  return CefExecuteProcess(main_args, app.get(), nullptr);
#endif
}

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF
