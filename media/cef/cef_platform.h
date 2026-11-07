#pragma once

// CefPlatform — per-platform glue for the CEF OSR browser process (T1).
//
// Split out of cef_runtime.cpp so each OS ships its own deployment-layout /
// library-loading logic in a dedicated translation unit (no macOS headers in
// the Linux path, no Linux headers in the macOS path):
//
//   cef_platform_common.cc — single-binary subprocess entry shared by both
//                            platforms (Linux zygote/renderer/gpu relaunch
//                            render-demo itself with --type=...; macOS uses
//                            separate Helper.app bundles so this is a no-op
//                            path there in practice).
//   cef_platform_mac.cc    — cefsimple bundle layout: framework + Helper under
//                            Contents/Frameworks, dynamic framework load via
//                            CefScopedLibraryLoader, _NSGetArgc/_NSGetArgv.
//   cef_platform_linux.cc  — flat distro layout: libcef.so + pak/locales
//                            beside the executable (copied by POST_BUILD),
//                            link-time load (libcef.so linked via
//                            CEF_STANDARD_LIBS), /proc/self/cmdline main
//                            args, own executable as browser_subprocess_path.
//
// Compile gate: XMEDIA_HAS_CEF (CMake ENABLE_CEF + CEF_ROOT, see
// CMakeLists.txt discovery gate). This header stays CEF-header-free so non-CEF
// TUs (render_demo.cc) can include it under their own #if without needing the
// CEF include path.

#include <string>

#include "cef_runtime.h"  // CefDeployPaths (CEF-header-free)

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

namespace xmedia {
namespace cef {

// Resolve CEF deployment paths for the running executable.
// macOS: .app bundle — framework_dir = Contents/Frameworks, subprocess_path =
//   "<basename> Helper" bundle executable beside Chromium Embedded Framework.
// Linux: flat cefsimple layout — subprocess_path = own executable (single
//   binary, Chromium relaunches it with --type=...), framework_dir = first
//   directory containing libcef.so among { exe dir, $MC_CEF_RUNTIME_DIR }.
// Returns false when the layout cannot be resolved (HTML layers unavailable).
bool CefPlatformDeployPaths(CefDeployPaths* out);

// Load/verify the CEF library before CefInitialize.
// macOS: dynamic load of "Chromium Embedded Framework.framework" from
//   framework_dir (fallback: bundle-relative LoadInMain).
// Linux: libcef.so is linked at build time (CEF_STANDARD_LIBS) — always true.
bool CefPlatformLoadFramework(const std::string& framework_dir);

// Process-wide (argc, argv) for CefInitialize's browser-process CefMainArgs.
// macOS: crt_externs _NSGetArgc/_NSGetArgv. Linux: /proc/self/cmdline.
// Returns false if unavailable; the caller then uses a default-constructed
// CefMainArgs (CefInitialize still works for the browser process).
bool CefPlatformBrowserMainArgs(int* out_argc, char*** out_argv);

// True when this process was launched as a CEF subprocess (argv contains
// "--type=..."). Linux single-binary mode: Chromium relaunches render-demo.
bool CefPlatformIsSubprocess(int argc, char** argv);

// Run the subprocess role (CefExecuteProcess) and return its exit code.
// Must be called from the top of main() before any other initialization
// (GLFW, glog, FFmpeg, ...) — the subprocess never runs the host pipeline.
int CefPlatformSubprocessMain(int argc, char** argv);

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF
