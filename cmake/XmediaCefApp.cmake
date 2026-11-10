# XmediaCefApp.cmake — CEF macOS .app bundle 组装（CEF 子进程契约）。
#
# macOS 的 CEF 子进程（renderer/GPU/network）要求宿主是 .app bundle：
# 框架加载、Helper 解析、Resources 定位都在 bundle 内完成（dyld 两级
# 命名空间不做 dynamic_lookup）。所有要在 macOS 上跑 CEF 的目标——
# 测试二进制、示例——都经这个函数组装：
#
#   <target>.app/
#     Contents/MacOS/<target>                                  ← 主二进制
#     Contents/Frameworks/Chromium Embedded Framework.framework
#     Contents/Frameworks/<target> Helper.app/.../<target> Helper
#
# 单二进制子进程模型：main() 拦截 --type= 并委托 CefExecuteProcess，
# Helper 二进制即主二进制的拷贝。资源不单独拷：CEF 156 的 macOS distro
# 把 icudtl.dat/*.pak 放在 framework 的 Resources/ 里（随 framework 目录
# 整棵拷贝），运行时 resources_dir_path 指向 framework 内部。
#
# 不用 MACOSX_BUNDLE 属性：CMAKE_RUNTIME_OUTPUT_DIRECTORY 与 bundle 路径
# 冲突，MacOS 目录会空（实测）。add_test 指向 bundle 内的二进制。
#
# 调用前提：XMEDIA_CEF_ROOT 已由 media/CMakeLists.txt 解析且
# XMEDIA_HAS_CEF=1。

function(xmedia_assemble_cef_app target)
  if(NOT APPLE)
    return()
  endif()
  if(NOT XMEDIA_HAS_CEF)
    message(FATAL_ERROR "xmedia_assemble_cef_app(${target}) without XMEDIA_HAS_CEF")
  endif()
  set(_app "$<TARGET_FILE_DIR:${target}>/${target}.app")
  set(_bin "${_app}/Contents/MacOS/${target}")
  set(_fw "${_app}/Contents/Frameworks")
  add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_app}/Contents/MacOS"
      COMMAND ${CMAKE_COMMAND} -E copy
          "$<TARGET_FILE:${target}>" "${_bin}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${XMEDIA_CEF_ROOT}/Release/Chromium Embedded Framework.framework"
          "${_fw}/Chromium Embedded Framework.framework"
      COMMAND ${CMAKE_COMMAND} -E make_directory
          "${_fw}/${target} Helper.app/Contents/MacOS"
      COMMAND ${CMAKE_COMMAND} -E copy
          "$<TARGET_FILE:${target}>"
          "${_fw}/${target} Helper.app/Contents/MacOS/${target} Helper"
      COMMENT "Assembling ${target}.app with CEF framework"
      VERBATIM)
endfunction()
