// CefPlatform — Linux implementation (browser process side).
//
// T1 platform split. Linux deployment follows cefsimple's flat layout: the
// CEF distro runtime files (libcef.so, icudtl.dat, *.pak, locales/, v8
// snapshots) live beside the executable — copied there by the render-demo
// POST_BUILD step from the distro's Release/ + Resources/ directories.
//
// Single-binary subprocess: Chromium relaunches this executable as
// zygote / gpu / renderer with `--type=<role>`; browser_subprocess_path is
// set to our own path (see CefPlatformDeployPaths) and render_demo main()
// delegates to CefPlatformSubprocessMain before any host initialization.
//
// Unlike macOS, libcef.so is linked at build time (CEF_STANDARD_LIBS), so
// there is no dlopen step. The X11/Wayland display stack is still required by
// the Chromium compositor at runtime (see docs/guides/LinuxCefBake.md).

#include "base/logging.h"
#include "cef_platform.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF && defined(__linux__)

#include <limits.h>
#include <stdlib.h>
#include <unistd.h>

#include <fstream>
#include <string>
#include <vector>

namespace xmedia {
namespace cef {
namespace {

bool ReadSelfExePath(std::string* out) {
  char buf[PATH_MAX];
  const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n <= 0) {
    return false;
  }
  buf[n] = '\0';
  // Resolve symlinks so the subprocess path matches the real install location
  // (build/render-demo is often a symlink into render-demo.app on macOS; on
  // Linux keep the same discipline for wrapper scripts).
  char real_buf[PATH_MAX];
  if (::realpath(buf, real_buf) != nullptr) {
    out->assign(real_buf);
  } else {
    out->assign(buf);
  }
  return true;
}

bool DirHasFile(const std::string& dir, const std::string& name) {
  return ::access((dir + "/" + name).c_str(), F_OK) == 0;
}

}  // namespace

bool CefPlatformDeployPaths(CefDeployPaths* out) {
  if (!out) {
    return false;
  }
  out->framework_dir.clear();
  out->subprocess_path.clear();

  std::string exe_path;
  if (!ReadSelfExePath(&exe_path)) {
    LOG(ERROR) << "[CefRuntime] Linux: cannot resolve /proc/self/exe\n";
    return false;
  }
  const auto slash = exe_path.find_last_of('/');
  if (slash == std::string::npos) {
    return false;
  }
  const std::string exe_dir = exe_path.substr(0, slash);

  // Single-binary subprocess (cefsimple convention): point
  // browser_subprocess_path at ourselves; render_demo main() detects the
  // relaunch (--type=...) and delegates to CefExecuteProcess.
  out->subprocess_path = exe_path;

  // framework_dir: directory containing libcef.so. Checked beside the
  // executable first (POST_BUILD deploy), then $MC_CEF_RUNTIME_DIR for
  // system installs / custom deployments.
  if (DirHasFile(exe_dir, "libcef.so")) {
    out->framework_dir = exe_dir;
    return true;
  }
  const char* env_dir = std::getenv("MC_CEF_RUNTIME_DIR");
  if (env_dir && *env_dir && DirHasFile(env_dir, "libcef.so")) {
    out->framework_dir = env_dir;
    return true;
  }
  LOG(ERROR) << "[CefRuntime] Linux: libcef.so not found beside "
            << exe_path << " nor under $MC_CEF_RUNTIME_DIR; run the CMake "
               "POST_BUILD deploy or set MC_CEF_RUNTIME_DIR\n";
  return false;
}

bool CefPlatformLoadFramework(const std::string& framework_dir) {
  // libcef.so is linked at build time via CEF_STANDARD_LIBS; no dlopen step
  // on Linux (contrast: macOS loads the framework dynamically because of the
  // sandbox model). Keep the hook so callers stay platform-uniform.
  (void)framework_dir;
  return true;
}

bool CefPlatformBrowserMainArgs(int* out_argc, char*** out_argv) {
  // The browser process needs real (argc, argv) for Chromium's command line
  // (ozone switches, logging, our --html-bake flags, ...). There is no
  // crt_externs on Linux: read /proc/self/cmdline (NUL-separated) instead of
  // threading main()'s argv down into CefRuntime.
  static std::vector<std::string> s_args;
  static std::vector<char*> s_argv;
  if (s_argv.empty()) {
    std::ifstream in("/proc/self/cmdline", std::ios::in | std::ios::binary);
    std::string tok;
    while (std::getline(in, tok, '\0')) {
      if (tok.empty()) {
        break;  // trailing NUL (or double-NUL separator)
      }
      s_args.push_back(tok);
    }
    s_argv.reserve(s_args.size());
    for (auto& arg : s_args) {
      s_argv.push_back(&arg[0]);
    }
  }
  if (s_argv.empty()) {
    return false;
  }
  if (out_argc) {
    *out_argc = static_cast<int>(s_argv.size());
  }
  if (out_argv) {
    *out_argv = s_argv.data();
  }
  return true;
}

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF && __linux__
