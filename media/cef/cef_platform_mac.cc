// CefPlatform — macOS implementation (browser process side).
//
// Moved from cef_runtime.cpp (T1 platform split). macOS deployment follows
// cefsimple: the host runs inside a .app bundle and Chromium spawns separate
// Helper.app bundles; Chromium Embedded Framework.framework is dlopen'ed (not
// linked) because of the macOS sandbox model.
//
// Layout resolved by CefPlatformDeployPaths (see CMake POST_BUILD):
//   <App>.app/Contents/MacOS/<exe>
//   <App>.app/Contents/Frameworks/Chromium Embedded Framework.framework
//   <App>.app/Contents/Frameworks/<exe> Helper[.suffix].app/Contents/MacOS/...

#include "base/logging.h"
#include "cef_platform.h"

// __APPLE__ (compiler builtin), not OS_MAC: the CEF macros (cef_build.h) are
// not included before the gate, and this TU must not compile away silently on
// a CEF-enabled macOS build (mirror: cef_platform_linux.cc uses __linux__).
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF && defined(__APPLE__)

#include <crt_externs.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <unistd.h>

#include <memory>

#include "include/wrapper/cef_library_loader.h"

namespace xmedia {
namespace cef {
namespace {

std::string SelfExecutablePath() {
  uint32_t size = 0;
  if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
    return std::string();
  }
  std::unique_ptr<char[]> buf(new char[size]);
  if (_NSGetExecutablePath(buf.get(), &size) != 0) {
    return std::string();
  }
  // Resolve symlinks (build/render-demo → …/MacOS/render-demo) so Frameworks
  // is found relative to the real bundle, not the symlink's parent.
  char real_buf[PATH_MAX];
  if (realpath(buf.get(), real_buf) != nullptr) {
    return std::string(real_buf);
  }
  return std::string(buf.get());
}

}  // namespace

bool CefPlatformDeployPaths(CefDeployPaths* out) {
  if (!out) {
    return false;
  }
  out->framework_dir.clear();
  out->subprocess_path.clear();

  // cefsimple-style:
  //   <App>.app/Contents/MacOS/<exe>
  //   <App>.app/Contents/Frameworks/Chromium Embedded Framework.framework
  //   <App>.app/Contents/Frameworks/<exe> Helper.app/...
  // Fallback: flat siblings next to a non-bundle executable (legacy).
  const std::string exe_path = SelfExecutablePath();
  if (exe_path.empty()) {
    return false;
  }
  const auto slash = exe_path.find_last_of('/');
  if (slash == std::string::npos) {
    return false;
  }
  const std::string exe_dir = exe_path.substr(0, slash);
  const std::string exe_base = exe_path.substr(slash + 1);
  const std::string helper_name = exe_base + " Helper";

  std::string frameworks_dir = exe_dir;
  const std::string mac_os_suffix = "/Contents/MacOS";
  if (exe_dir.size() >= mac_os_suffix.size() &&
      exe_dir.compare(exe_dir.size() - mac_os_suffix.size(),
                      mac_os_suffix.size(), mac_os_suffix) == 0) {
    frameworks_dir =
        exe_dir.substr(0, exe_dir.size() - mac_os_suffix.size()) +
        "/Contents/Frameworks";
  }

  const std::string helper_exec = frameworks_dir + "/" + helper_name +
                                  ".app/Contents/MacOS/" + helper_name;
  const std::string framework_bundle =
      frameworks_dir + "/Chromium Embedded Framework.framework";
  if (access(framework_bundle.c_str(), F_OK) != 0 ||
      access(helper_exec.c_str(), F_OK) != 0) {
    return false;
  }

  out->framework_dir = frameworks_dir;
  out->subprocess_path = helper_exec;
  return true;
}

bool CefPlatformLoadFramework(const std::string& framework_dir) {
  // Prefer explicit path from Init (POST_BUILD copy beside host executable).
  // Fallback: CefScopedLibraryLoader::LoadInMain expects
  //   <exe>/../Frameworks/Chromium Embedded Framework.framework/...
  if (!framework_dir.empty()) {
    const std::string lib_path =
        framework_dir +
        "/Chromium Embedded Framework.framework/Chromium Embedded Framework";
    // cef_load_library is NOT idempotent: it returns 0 when the framework is
    // already loaded (g_libcef_handle set), so a host that pre-loaded CEF
    // before starting a layer would be misread as a load failure. Probe with
    // RTLD_NOLOAD first — a non-null handle means the framework is resident.
    void* resident = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
    if (resident != nullptr) {
      return true;
    }
    if (cef_load_library(lib_path.c_str())) {
      return true;
    }
    LOG(ERROR) << "[CefRuntime] cef_load_library failed for " << lib_path
              << "\n";
  }
  // Stack-allocated loader unloads on destroy — keep library loaded for the
  // process lifetime by using cef_load_library only above, or LoadInMain and
  // intentionally leaking a heap loader until Shutdown.
  static CefScopedLibraryLoader* s_loader = nullptr;
  if (!s_loader) {
    s_loader = new CefScopedLibraryLoader();
  }
  if (s_loader->LoadInMain()) {
    return true;
  }
  LOG(ERROR) << "[CefRuntime] LoadInMain failed (Chromium Embedded "
               "Framework.framework not found beside executable or under "
               "../Frameworks)\n";
  return false;
}

bool CefPlatformBrowserMainArgs(int* out_argc, char*** out_argv) {
  // crt_externs: process-wide argument vector (matches CefMainArgs(
  // *_NSGetArgc(), *_NSGetArgv()) previously used in CefRuntime::Init).
  if (out_argc) {
    *out_argc = *_NSGetArgc();
  }
  if (out_argv) {
    *out_argv = *_NSGetArgv();
  }
  return true;
}

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF && OS_MAC
