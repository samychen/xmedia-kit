// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_EXAMPLES_HTML_BAKE_ARGS_H_
#define XMEDIA_EXAMPLES_HTML_BAKE_ARGS_H_

// html_bake 的命令行参数与解析。
//
// 拆出本文件只为让 html_bake.cc 专注于烘焙管线本身：这段是纯数据变换，
// 不碰 GL、CEF、编码器，输入是 argv，输出是一个值对象。解析失败时
// ParseArgs 打印用法并返回 false，调用方直接退出——含义与拆开前一致。

#include <string>

namespace xmedia {
namespace examples {

struct BakeArgs {
  std::string url;
  std::string out = "html_bake_out.mkv";
  double seconds = 1.0;
  int fps = 10;
  int width = 320;
  int height = 240;
  std::string codec = "mjpeg";
  // bake 时钟（MC html_bake_clock 对齐）：virtual_time = VT 步进；
  // seek = 页面 window.seek(t_ms) 契约（MC 默认）；live = 墙钟。
  std::string clock = "virtual_time";
  // seek/live 时钟下每帧等新 paint 的时长（ms）：页面在 GSAP/字体
  // 加载期 paint 会迟到，300ms 窗口错过即把旧帧写进产物（好-坏震荡
  // 的根因）——Python 管线用 1500 提高确定性。
  int paint_wait_ms = 300;
  // 分块数：时间线均分为 N 窗，每窗独立烘焙（VT 从 0 快进到窗起点——
  // 合成确定性使任意边界成为可能，无 keyframe 对齐约束），Concat 无损
  // 拼接。1 = 单块直通。
  int chunks = 1;
  // 页面数据注入（window.update 契约）：首帧后注入一次。
  std::string data_json;
  // Lottie 叠加层：路径 + 绘制 z（默认垫在 HTML 元素之下）。
  std::string lottie_path;
  int lottie_z = -1;
  double lottie_speed = 1.0;
  int lottie_loop = 1;
};

// 解析 argv。|prog| 用于用法提示里的程序名。失败（缺参数、未知开关、
// 数量不足）时向 stderr 打印可操作的说明并返回 false。
bool ParseBakeArgs(int argc, char** argv, BakeArgs* out);

}  // namespace examples
}  // namespace xmedia

#endif  // XMEDIA_EXAMPLES_HTML_BAKE_ARGS_H_
