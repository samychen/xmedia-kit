#!/bin/bash
# Lottie 支持：rlottie（Samsung，MIT）静态库最小化构建脚本。
# 移植自 MediaComponent tools/build_rlottie.sh（T7），离线可重建：
# SOURCE_DIR 已有 rlottie-<ver>/ 时跳过下载。
# 产出：tools/build/lib/librlottie.a + tools/build/include/rlottie/*.h
#       （opengl_render 的 `lottie` source 类型在找到该产物时才启用，
#        对齐 CEF 的 MEDIA_COMPONENT_HAS_CEF 降级模式。）
# 执行: ./tools/build_rlottie.sh
# 覆盖示例:
#   RLOTTIE_VER=0.2 ./build_rlottie.sh              # 默认版本
#   FETCH_CMD="curl -fsSL" ./build_rlottie.sh       # 换下载器
#
# 网络说明（2026-11-03 本机实测）：github.com 直连 git clone / codeload 超时，
# raw.githubusercontent.com 可达；脚本按「直连 → ghfast.top 镜像」顺序尝试 tarball。
# 已有 tools/deps/rlottie-<ver>/ 源码目录时跳过下载（离线可重建）。
set -e
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR=${INSTALL_DIR:-${SCRIPT_DIR}/build}
SOURCE_DIR=${SOURCE_DIR:-${SCRIPT_DIR}/deps}

RLOTTIE_VER="${RLOTTIE_VER:-0.2}"
SRC_NAME="rlottie-${RLOTTIE_VER}"
TARBALL="${SRC_NAME}.tar.gz"
SRC_DIR="${SOURCE_DIR}/${SRC_NAME}"
PREFIX="${INSTALL_DIR}"

# CMake：项目约定 4.x（/usr/local/opt/cmake/bin/cmake），PATH 里找不到时回退。
if [ -x /usr/local/opt/cmake/bin/cmake ]; then
  CMAKE=/usr/local/opt/cmake/bin/cmake
elif command -v cmake >/dev/null 2>&1; then
  CMAKE=cmake
else
  echo "ERROR: cmake not found (need >= 3.21 for CMAKE_POLICY_VERSION_MINIMUM flow)" >&2
  exit 1
fi

mkdir -p "${SOURCE_DIR}"

fetch_tarball() {
  local out="$1"; shift
  for url in "$@"; do
    echo "[rlottie] trying ${url}"
    if curl -fsSL --max-time 120 -o "${out}" "${url}"; then
      [ -s "${out}" ] && return 0
    fi
    rm -f "${out}"
  done
  return 1
}

if [ ! -f "${SRC_DIR}/CMakeLists.txt" ]; then
  fetch_tarball "${SOURCE_DIR}/${TARBALL}" \
    "https://github.com/Samsung/rlottie/archive/refs/tags/v${RLOTTIE_VER}.tar.gz" \
    "https://ghfast.top/https://github.com/Samsung/rlottie/archive/refs/tags/v${RLOTTIE_VER}.tar.gz" \
    "https://gh-proxy.com/https://github.com/Samsung/rlottie/archive/refs/tags/v${RLOTTIE_VER}.tar.gz"
  tar -xzf "${SOURCE_DIR}/${TARBALL}" -C "${SOURCE_DIR}"
  rm -f "${SOURCE_DIR}/${TARBALL}"
fi

BUILD_DIR="${SRC_DIR}/build-static"
# rlottie 上游 cmake_minimum_required(3.3)：CMake 4.x 拒绝 <3.5，用
# CMAKE_POLICY_VERSION_MINIMUM=3.5 免改上游源码。
# 最小化：静态库；关掉插件 dlopen（LOTTIE_MODULE）/线程锁（LOTTIE_THREAD，
# 渲染固定在引擎 Graphics 线程）/模型缓存（LOTTIE_CACHE）/测试与示例。
"${CMAKE}" -S "${SRC_DIR}" -B "${BUILD_DIR}" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBUILD_SHARED_LIBS=OFF \
  -DLOTTIE_MODULE=OFF \
  -DLOTTIE_THREAD=OFF \
  -DLOTTIE_CACHE=OFF \
  -DLOTTIE_TEST=OFF \
  -DLOTTIE_CCACHE=OFF \
  -DLOTTIE_ASAN=OFF \
  -DCMAKE_BUILD_TYPE=Release
"${CMAKE}" --build "${BUILD_DIR}" -j8

# 头文件手工安装（rlottie 的 install 规则会带 example，绕开）：inc/ 下 3 个头。
mkdir -p "${PREFIX}/lib" "${PREFIX}/include/rlottie"
cp -f "${BUILD_DIR}/librlottie.a" "${PREFIX}/lib/librlottie.a"
cp -f "${SRC_DIR}/inc/rlottie.h" "${SRC_DIR}/inc/rlottiecommon.h" \
   "${SRC_DIR}/inc/rlottie_capi.h" "${PREFIX}/include/rlottie/"

echo "[rlottie] installed: ${PREFIX}/lib/librlottie.a + ${PREFIX}/include/rlottie/"
