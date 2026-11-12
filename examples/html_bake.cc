// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// html_bake — HyperFrames html_bake 对齐：HTML → 视频离线烘焙。
//
//   ./html_bake --url file:///.../smoke.html --out out.mkv
//               [--seconds 1.0] [--fps 10] [--width 320] [--height 240]
//               [--codec mjpeg]
//
// 管线：CEF OSR（VirtualTime bake 时钟）→ 每输出帧 RequestVtStep →
// OnPaint 帧进 GlElementComposer（元素拆纹理：data-* 元素各自成为场景
// 层，data-z 排序）→ GlScene 合成 → 读回 BGRA → 转 I420 → FFmpegVideoEncoder
// → FFmpegEncodeMuxer → 容器。
//
// 为什么 VirtualTime：墙钟驱动下 HTML 的动画/时序依赖真实时间流逝，帧
// 间隔抖动会进输出；VT 每步精确前进一帧间隔，文档时刻 t = 帧序号 *
// (1000/fps)，输出确定性可复现。
//
// 只在 macOS 的 .app bundle 里跑（CEF 子进程契约）：CMake POST_BUILD 组装
// <out>/<configuration>/html_bake.app，ctest/示例都从 bundle 内的二进制
// 启动。GTK/Linux 走平铺布局，无需组装。
//
// 已知边界（如实登记）：
//  * 元素出生效窗的首帧可能仍缺该元素——CEF 渲染器对 visibility 恢复的
//    重绘惰性（隐藏方向精确：非活动元素不被采样；内容验证式重绘可缩小
//    但不能消除）。HyperFrames 原版无此问题：其页面框架自持渲染。
//  * 平铺光栅裁切对重叠元素透底：非重叠布局是精确前提。

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "base/logging.h"
#include "media/base/video_frame.h"
#include "media/cef/cef_app.h"
#include "media/cef/cef_layer_source.h"

#if defined(XMEDIA_HAS_LOTTIE) && XMEDIA_HAS_LOTTIE
#include "media/lottie/lottie_animation.h"
#include "media/lottie/lottie_clock.h"
#endif
#include "media/cef/cef_runtime.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/ffmpeg/video_convert.h"
#include "media/gl/gl_element_composer.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "media/media_export.h"
#include "media/transcode/ffmpeg_concat_job.h"
#include "media/transcode/ffmpeg_encode_muxer.h"
#include "media/transcode/ffmpeg_video_encoder.h"
#include "platform/glfw/glfw_gl_context.h"
#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"
#include "render/gl/gl_api.h"

#if !(defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF)
int main() {
  std::printf("html_bake: built without XMEDIA_ENABLE_CEF\n");
  return 0;
}
#else

namespace ff = xmedia::media::ffmpeg;

// Helper 子进程的 framework 装载（CEF 延迟加载契约；与冒烟测试同款）。
namespace xmedia::cef {
bool LoadFrameworkForHelperProcess() {
  static std::unique_ptr<CefScopedLibraryLoader> loader;
  if (!loader) {
    loader = std::make_unique<CefScopedLibraryLoader>();
  }
  return loader->LoadInHelper();
}
}  // namespace xmedia::cef

namespace {

using namespace xmedia;  // NOLINT -- an example, not the SDK.
using namespace xmedia::media;  // NOLINT

struct Args {
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

Args ParseArgs(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string v = i + 1 < argc ? argv[i + 1] : "";
    if (std::strcmp(argv[i], "--url") == 0 && !v.empty()) {
      a.url = v;
      ++i;
    } else if (std::strcmp(argv[i], "--out") == 0 && !v.empty()) {
      a.out = v;
      ++i;
    } else if (std::strcmp(argv[i], "--seconds") == 0) {
      a.seconds = std::atof(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--fps") == 0) {
      a.fps = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--width") == 0) {
      a.width = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--height") == 0) {
      a.height = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--codec") == 0 && !v.empty()) {
      a.codec = v;
      ++i;
    } else if (std::strcmp(argv[i], "--clock") == 0 && !v.empty()) {
      a.clock = v;  // virtual_time | seek | live
      ++i;
    } else if (std::strcmp(argv[i], "--chunks") == 0) {
      a.chunks = std::max(1, std::atoi(v.c_str()));
      ++i;
    } else if (std::strcmp(argv[i], "--data") == 0 && !v.empty()) {
      a.data_json = v;
      ++i;
    } else if (std::strcmp(argv[i], "--lottie") == 0 && !v.empty()) {
      a.lottie_path = v;
      ++i;
    } else if (std::strcmp(argv[i], "--lottie-z") == 0) {
      a.lottie_z = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--lottie-speed") == 0) {
      a.lottie_speed = std::atof(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--lottie-loop") == 0) {
      a.lottie_loop = std::atoi(v.c_str());
      ++i;
    }
  }
  return a;
}

// 等到 OSR 发布了新帧（VT 步进的确定性产出）：caller-thread pump 模式下
// 主线程自己驱动 CEF——Pump 处理 UI 任务（含 VT credit 消费与 OnPaint
// 投递），pump 线程模式下这由 facade 的后台线程做。超时返回 false——
// bake 是确定性管线，丢帧 = 时间线错位，直接失败而不是带病继续。
bool WaitNewFrame(CefLayerSource* layer, const void* previous,
                  int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited += 5) {
    layer->Pump();
    const auto frame = layer->last_frame();
    if (frame && frame.get() != previous) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  // CEF 单二进制子进程模型：Chromium 以 --type=... 重启本二进制作为
  // renderer/GPU/network 子进程。拦截后加载 framework 并委托
  // CefExecuteProcess——不拦截的话，子进程跑全量 main（GLFW + 烘焙），
  // GPU 子进程必然崩，CEF 报 "GPU process isn't usable" 后整体退出。
  for (int i = 0; i < argc; ++i) {
    if (std::strncmp(argv[i], "--type=", 7) == 0) {
      if (!xmedia::cef::LoadFrameworkForHelperProcess()) {
        std::fprintf(stderr, "html_bake: helper framework load failed\n");
        return 1;
      }
      CefMainArgs main_args(argc, argv);
      CefRefPtr<xmedia::cef::MediaCefApp> app(new xmedia::cef::MediaCefApp);
      return CefExecuteProcess(main_args, app.get(), nullptr);
    }
  }
  const Args args = ParseArgs(argc, argv);
  if (args.url.empty()) {
    std::fprintf(stderr, "usage: html_bake --url <file:///...> --out <path> "
                         "[--seconds s] [--fps n] [--width w] [--height h] "
                         "[--codec name]\n");
    return 1;
  }

  // --- GL：隐藏窗口（离屏合成 + 读回），drawable 尺寸为准（HiDPI）。
  if (glfwInit() != GLFW_TRUE) {
    std::fprintf(stderr, "html_bake: glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  GLFWwindow* window = glfwCreateWindow(args.width, args.height,
                                        "html_bake", nullptr, nullptr);
  if (window == nullptr) {
    std::fprintf(stderr, "html_bake: glfwCreateWindow failed\n");
    return 1;
  }
  auto ctx = std::make_unique<GlfwGLContext>(window);
  if (!ctx->MakeCurrent()) {
    std::fprintf(stderr, "html_bake: MakeCurrent failed\n");
    return 1;
  }
  GlPresenter presenter(std::move(ctx));
  if (!presenter.Init()) {
    std::fprintf(stderr, "html_bake: presenter init failed\n");
    return 1;
  }
  std::string gl_error;
  auto scene = GlScene::Create(*presenter.api(), &gl_error);
  if (!scene) {
    std::fprintf(stderr, "html_bake: scene create failed: %s\n",
                 gl_error.c_str());
    return 1;
  }
  int drawable_w = args.width;
  int drawable_h = args.height;
  glfwGetFramebufferSize(window, &drawable_w, &drawable_h);

  // --- 编码器 + 封装（bundle 外的产物路径直接写）。
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params ep;
  ep.codec_name = args.codec;
  ep.width = drawable_w;
  ep.height = drawable_h;
  ep.fps_num = args.fps;
  ep.fps_den = 1;
  if (!encoder.Initialize(ep)) {
    std::fprintf(stderr, "html_bake: encoder %s init failed\n",
                 args.codec.c_str());
    return 1;
  }
  FFmpegEncodeMuxer muxer;
  if (!muxer.Open(args.out)) {
    std::fprintf(stderr, "html_bake: cannot open output %s\n",
                 args.out.c_str());
    return 1;
  }
  FFmpegEncodeMuxer::VideoStreamParams sp;
  sp.codec_name = args.codec;
  sp.width = drawable_w;
  sp.height = drawable_h;
  const AVRational tb = encoder.time_base();
  sp.time_base_num = tb.num;
  sp.time_base_den = tb.den;
  sp.extradata = encoder.codec_parameters()
                     ? std::vector<uint8_t>(
                           encoder.codec_parameters()->extradata,
                           encoder.codec_parameters()->extradata +
                               encoder.codec_parameters()->extradata_size)
                     : std::vector<uint8_t>();
  if (muxer.AddVideoStream(sp) < 0) {
    std::fprintf(stderr, "html_bake: add video stream failed\n");
    return 1;
  }
  // BGRA 读回 → I420 编码输入：一次性配好，全帧复用。
  ff::VideoConverter converter;
  if (!converter.Configure(drawable_w, drawable_h, AV_PIX_FMT_BGRA,
                           drawable_w, drawable_h, AV_PIX_FMT_YUV420P)) {
    std::fprintf(stderr, "html_bake: BGRA->I420 conversion setup failed\n");
    return 1;
  }

  // --- CEF OSR：VT bake 时钟。
  CefLayerSource::Config config;
  config.name = "html_bake";
  config.width = args.width;
  config.height = args.height;
  config.fps = args.fps;
  config.url = args.url;
  config.bake_virtual_time = args.clock == "virtual_time";
  // GLFW 占据主线程 run loop：CEF 的 Init/pump 必须同在主线程（见
  // pump_on_caller_thread 的注释——pump 线程模式实测死锁在 CefInitialize）。
  config.pump_on_caller_thread = true;
  CefLayerSource layer;
  if (!layer.Start(config)) {
    std::fprintf(stderr, "html_bake: CEF layer failed to start "
                         "(must run inside the .app bundle)\n");
    return 1;
  }
  // --synthetic：跳过 CEF 内容，用已知象限帧走完整 GL→读回→编码管线，
  // 定位方向性问题的归属（GL 侧 vs CEF 侧）。
  xmedia::base::scoped_refptr<VideoFrame> synthetic;
  std::vector<HtmlElementMetaEntry> synthetic_entries;
  if (getenv("BAKE_SYNTHETIC") != nullptr) {
    synthetic = VideoFrame::CreateBlackFrame(
        VideoFormat::kARGB, Size{320, 240}, Size{320, 240}, Rational{1, 1},
        xmedia::base::Milliseconds(0), xmedia::base::Milliseconds(100), 0);
    {
      auto data = synthetic->mutable_data(VideoFrame::kYPlane);
      const int32_t stride = synthetic->stride(VideoFrame::kYPlane);
      for (int y = 0; y < 240; ++y) {
        for (int x = 0; x < 320; ++x) {
          uint8_t* px =
              data.data() + static_cast<size_t>(y) * stride + x * 4;
          // 左上红 / 右下绿 / 其余白（内存序 B,G,R,A）
          if (x < 160 && y < 120) {
            px[0] = 0; px[1] = 0; px[2] = 255; px[3] = 255;
          } else if (x >= 160 && y >= 120) {
            px[0] = 0; px[1] = 255; px[2] = 0; px[3] = 255;
          } else {
            px[0] = 255; px[1] = 255; px[2] = 255; px[3] = 255;
          }
        }
      }
    }
    HtmlElementMetaEntry red;
    red.id = "red";
    red.ordinal = 0;
    red.x = 0; red.y = 0; red.w = 160; red.h = 120;
    red.visible = true;
    HtmlElementMetaEntry green;
    green.id = "green";
    green.ordinal = 1;
    green.x = 160; green.y = 120; green.w = 160; green.h = 120;
    green.visible = true;
    synthetic_entries = {red, green};
  }

  // 等首帧（页面加载 + 首次 paint）。
  if (!synthetic && !WaitNewFrame(&layer, nullptr, 15000)) {
    std::fprintf(stderr, "html_bake: no first frame in 15s\n");
    layer.Stop();
    return 1;
  }

  if (!args.data_json.empty()) {
    if (!layer.SetHtmlData(args.data_json)) {
      std::fprintf(stderr, "html_bake: --data rejected (invalid JSON)\n");
      return 1;
    }
  }

  // Lottie 叠加层（XMEDIA_HAS_LOTTIE）：独立场景层，z 垫在 HTML 元素下。
#if defined(XMEDIA_HAS_LOTTIE) && XMEDIA_HAS_LOTTIE
  std::unique_ptr<lottie::LottieAnimation> lottie_anim;
  GlScene::LayerId lottie_layer = GlScene::kNoLayer;
  if (!args.lottie_path.empty()) {
    lottie::LottieSourceParams lp;
    lp.path = args.lottie_path;
    lp.speed = args.lottie_speed;
    lp.loop = args.lottie_loop != 0;
    lottie_anim = lottie::LottieAnimation::Open(lp, std::max(drawable_w, drawable_h));
    if (lottie_anim) {
      lottie_layer = scene->AddVideoLayer(args.lottie_z);
      // Lottie 铺满内容矩形（与元素拆纹理同一内容映射）。
      scene->SetLayerRect(lottie_layer, 0.f, 0.f, 1.f, 1.f);
      std::fprintf(stderr, "html_bake: lottie %s — %d frames @ %.2f fps, "
                           "surface %dx%d\n",
                   args.lottie_path.c_str(), lottie_anim->total_frame(),
                   lottie_anim->src_fps(), lottie_anim->surface_width(),
                   lottie_anim->surface_height());
    }
  }
#endif

  // 时序预热：采集脚本经 OnLoadEnd 重注入，但 ExecuteJavaScript 的渲染
  // 进程落地相对 VT 步进有 ~2 帧热身期（实测前两帧时序不生效）。先丢两
  // 轮 t=0 的步进，DOM 可见性机制稳定后再开录。
  for (int prime = 0; prime < 2; ++prime) {
    if (args.clock == "virtual_time") {
      layer.ApplyTimeline(0);
      layer.RequestVtStep();
      if (!WaitNewFrame(&layer, layer.last_frame().get(), 5000)) {
        std::fprintf(stderr, "html_bake: warmup %d timed out\n", prime);
        layer.Stop();
        return 1;
      }
    } else {
      // seek/live：热身只需 JS 落地 + 一帧（内容不变时无 paint = 正常，
      // 短等待非致命——VT 步进不存在，不能等 5 秒）。
      layer.ApplyTimeline(0);
      layer.TickBakeClock(0);
      if (args.clock == "seek") {
        layer.AwaitSeekDone(0, 2000);
      }
      WaitNewFrame(&layer, layer.last_frame().get(), 300);
    }
  }

  // --- 元素拆纹理合成器（GlElementComposer 的生产消费方：bake 管线）。
  GlElementComposer::Config composer_config;
  composer_config.target_w = drawable_w;
  composer_config.target_h = drawable_h;
  composer_config.default_z = 0;
  GlElementComposer composer(composer_config);


  const int total_frames =
      static_cast<int>(args.seconds * args.fps + 0.5);
  int64_t encoded = 0;

  // 单帧烘焙：fast_forward=true 时只步进不合成/编码（分块的窗前快进）。
  // 光栅内容哈希：边界帧的重绘验证用（合成器提交状态可能落后数拍，
  // 单靠等待新帧无法区分"新帧"与"旧内容的重合成"）。
  uint64_t prev_raster_hash = 0;
  auto raster_hash = [](const VideoFrame& f) {
    const auto bytes = f.visible_data(VideoFrame::kYPlane);
    uint64_t h = 1469598103934665603ull;
    for (const uint8_t b : bytes) {
      h = (h ^ b) * 1099511628211ull;
    }
    return h;
  };
  auto bake_frame = [&](int f, bool record) -> bool {
    const int64_t t_ms =
        static_cast<int64_t>(f) * 1000 / std::max(1, args.fps);
    const void* previous = layer.last_frame().get();
    if (!synthetic) {
      // 统一时钟分发（TickBakeClock）：VT = credit/帧；seek = 同 t 去重
      // + 在飞门控的 window.seek；live = no-op。
      layer.TickBakeClock(t_ms);
      if (args.clock == "seek") {
        // seekDone 门（页面契约回执）；no_seek 页面回退预应用时序。
        if (!layer.AwaitSeekDone(t_ms, 2000)) {
          if (layer.seek_unsupported()) {
            layer.ApplyTimeline(t_ms);
            if (!layer.AwaitTimelineApplied(t_ms, 2000)) {
              std::fprintf(stderr, "html_bake: fallback ack %d miss\n",
                           f);
            }
          } else {
            // seekDone 超时：对齐 MC deadline 语义——带病继续。
            std::fprintf(stderr, "html_bake: seek %d done not in 2s\n", f);
          }
        }
      }
      // seek/live：页面只在内容变化时出帧（无变化 = 最后一次 paint 就
      // 是本帧内容）——短等待新 paint 以提高新鲜度，超时用当前帧，不
      // 致命。VT 模式每步强制重绘，长等待 + 致命。
      if (args.clock == "virtual_time") {
        if (!WaitNewFrame(&layer, previous, 5000)) {
          std::fprintf(stderr, "html_bake: paint %d timed out\n", f);
          return false;
        }
      } else {
        WaitNewFrame(&layer, previous, 300);
      }
      if (record && args.clock == "virtual_time") {
        // 边界帧（元素进/出生效窗）：内容验证式重绘。合成器提交预
        // 应用状态可能落后数拍，"来了新帧"不等于"新帧携带新状态"
        // ——反复无效化并等新帧，直到光栅内容与上一录制帧不同
        //（边界必然带来该区域内容变化）或有界重试耗尽。
        const auto entries = layer.SnapshotElements().entries();
        const int64_t dt = 1000 / std::max(1, args.fps);
        bool boundary = false;
        for (const auto& e : entries) {
          if (IsElementPresentAt(e.has_start, e.start_ms,
                                 e.has_duration, e.duration_ms,
                                 t_ms) !=
              IsElementPresentAt(e.has_start, e.start_ms,
                                 e.has_duration, e.duration_ms,
                                 t_ms - dt)) {
            boundary = true;
            break;
          }
        }
        if (boundary) {
          for (int retry = 0; retry < 5; ++retry) {
            if (raster_hash(*layer.last_frame()) != prev_raster_hash) {
              break;
            }
            layer.Invalidate();
            if (!WaitNewFrame(&layer, layer.last_frame().get(),
                              500)) {
              break;
            }
          }
        }
      }
      if (!record) {
        return true;
      }
    }
    const auto frame =
        synthetic ? synthetic : layer.last_frame();
    const auto elements = synthetic
                              ? synthetic_entries
                              : layer.SnapshotElements().entries();
    composer.Sync(*scene, frame.get(), elements, t_ms);
#if defined(XMEDIA_HAS_LOTTIE) && XMEDIA_HAS_LOTTIE
    if (lottie_anim && lottie_layer != GlScene::kNoLayer) {
      auto lf = lottie_anim->RenderAtMs(t_ms);
      if (lf) {
        scene->Upload(lottie_layer, *lf);
      }
    }
#endif
    if (!presenter.ComposeScene(*scene)) {
      std::fprintf(stderr, "html_bake: compose failed at frame %d\n", f);
      return false;
    }

    // 读回 BGRA（GL 默认帧缓冲，底上原点——VT 内容自顶向下渲染，读回
    // 的 y 翻转在这里处理：glReadPixels 的行序即 BGRA 帧的行序，翻不翻
    // 取决于约定；bake 输出以"画面方向与浏览器一致"为准）。
    std::vector<uint8_t> bgra(static_cast<size_t>(drawable_w) * drawable_h * 4);
    presenter.api()->context().ReadPixels(0, 0, drawable_w, drawable_h,
                                          GL_BGRA, GL_UNSIGNED_BYTE,
                                          bgra.data());
    // glReadPixels 行 0 = GL 底部；编码器要 top-down。原地翻转行序——
    // 不翻转则整帧上下颠倒（合成模式实测：红/绿象限上下互换）。
    {
      const size_t row_bytes = static_cast<size_t>(drawable_w) * 4;
      std::vector<uint8_t> tmp(row_bytes);
      for (int top = 0, bottom = drawable_h - 1; top < bottom; ++top, --bottom) {
        uint8_t* a = bgra.data() + static_cast<size_t>(top) * row_bytes;
        uint8_t* b = bgra.data() + static_cast<size_t>(bottom) * row_bytes;
        std::memcpy(tmp.data(), a, row_bytes);
        std::memcpy(a, b, row_bytes);
        std::memcpy(b, tmp.data(), row_bytes);
      }
    }

    // BGRA → I420 → VideoFrame。
    ff::FramePtr src = ff::MakeFrame();
    src->width = drawable_w;
    src->height = drawable_h;
    src->format = AV_PIX_FMT_BGRA;
    src->data[0] = bgra.data();
    src->linesize[0] = drawable_w * 4;
    auto out_frame = VideoFrame::CreateBlackFrame(
        VideoFormat::kI420, Size{drawable_w, drawable_h},
        Size{drawable_w, drawable_h}, Rational{1, 1},
        xmedia::base::Milliseconds(f * 1000 / std::max(1, args.fps)),
        xmedia::base::Milliseconds(1000 / std::max(1, args.fps)), 0);
    std::vector<uint8_t*> dst(4, nullptr);
    std::vector<int> dst_ls(4, 0);
    dst[0] = out_frame->mutable_data(VideoFrame::kYPlane).data();
    dst_ls[0] = out_frame->stride(VideoFrame::kYPlane);
    dst[1] = out_frame->mutable_data(VideoFrame::kUPlane).data();
    dst_ls[1] = out_frame->stride(VideoFrame::kUPlane);
    dst[2] = out_frame->mutable_data(VideoFrame::kVPlane).data();
    dst_ls[2] = out_frame->stride(VideoFrame::kVPlane);
    if (!converter.Convert(*src, dst.data(), dst_ls.data())) {
      std::fprintf(stderr, "html_bake: BGRA->I420 failed at frame %d\n", f);
      return false;
    }
    std::vector<EncodedPacket> packets;
    if (!encoder.Encode(std::move(out_frame), &packets)) {
      std::fprintf(stderr, "html_bake: encode failed at frame %d\n", f);
      return false;
    }
    for (const auto& pkt : packets) {
      if (!muxer.WritePacket(0, pkt)) {
        std::fprintf(stderr, "html_bake: muxer rejected packet at frame %d\n",
                     f);
        return false;
      }
      ++encoded;
    }
    // 预应用下一帧的时序并等 ACK：DOM 在下一次步进前就处于目标
    // 状态——帧 f 的绘制精确携带 visibility(t_f)，边界帧无合成
    // 管线延迟（ExecuteJavaScript 与提交的同拍性不可依赖，提前
    // 一帧应用把顺序钉死）。
    const int64_t next_t =
        static_cast<int64_t>(f + 1) * 1000 / std::max(1, args.fps);
    prev_raster_hash = raster_hash(*frame);
    if (args.clock == "virtual_time") {
      layer.ApplyTimeline(next_t);
      if (!layer.AwaitTimelineApplied(next_t, 2000)) {
        std::fprintf(stderr, "html_bake: pre-apply %d ack miss\n",
                     f);
      }
    }
    return true;
  };

  // 分块烘焙：每块独立编码器/封装器（块间零依赖），窗前 VT 快进——
  // 合成是确定性的，任意边界精确，无 keyframe 对齐约束（这是与
  // ChunkedTranscode 对已有输入转码的本质差别）。
  const std::string ext = [&] {
    const auto dot = args.out.find_last_of('.');
    return dot == std::string::npos ? ".mkv" : args.out.substr(dot);
  }();
  const int per = (total_frames + args.chunks - 1) / args.chunks;
  std::vector<std::string> chunk_paths;
  for (int c = 0; c < args.chunks; ++c) {
    const int begin = c * per;
    const int end = std::min(begin + per, total_frames);
    if (begin >= end) {
      break;
    }
    const bool single = args.chunks == 1;
    const std::string chunk_out =
        single ? args.out
               : args.out + ".chunk" + std::to_string(c) + ext;
    auto chunk_encoder = std::make_unique<FFmpegVideoEncoder>();
    FFmpegVideoEncoder::Params cep = ep;
    if (!chunk_encoder->Initialize(cep)) {
      std::fprintf(stderr, "html_bake: chunk %d encoder init failed\n", c);
      layer.Stop();
      return 1;
    }
    auto chunk_muxer = std::make_unique<FFmpegEncodeMuxer>();
    if (!chunk_muxer->Open(chunk_out)) {
      std::fprintf(stderr, "html_bake: cannot open %s\n", chunk_out.c_str());
      layer.Stop();
      return 1;
    }
    FFmpegEncodeMuxer::VideoStreamParams csp = sp;
    const AVRational ctb = chunk_encoder->time_base();
    csp.time_base_num = ctb.num;
    csp.time_base_den = ctb.den;
    csp.extradata = chunk_encoder->codec_parameters()
                        ? std::vector<uint8_t>(
                              chunk_encoder->codec_parameters()->extradata,
                              chunk_encoder->codec_parameters()->extradata +
                                  chunk_encoder->codec_parameters()
                                      ->extradata_size)
                        : std::vector<uint8_t>();
    if (chunk_muxer->AddVideoStream(csp) < 0) {
      std::fprintf(stderr, "html_bake: chunk %d add stream failed\n", c);
      layer.Stop();
      return 1;
    }

    // 窗前快进（VT 从 0 确定性推进到窗起点），然后逐帧录制。录制前
    // 预应用块首帧的时序并等 ACK。
    if (args.clock == "virtual_time") {
      const int64_t begin_t =
          static_cast<int64_t>(begin) * 1000 /
          std::max(1, args.fps);
      layer.ApplyTimeline(begin_t);
      if (!layer.AwaitTimelineApplied(begin_t, 2000)) {
        std::fprintf(stderr,
                     "html_bake: chunk %d pre-apply ack miss\n", c);
      }
    }
    bool ok = true;
    for (int f = 0; ok && f < begin; ++f) {
      ok = bake_frame(f, /*record=*/false);
    }
    for (int f = begin; ok && f < end; ++f) {
      // 块内时间戳从 0 起：Concat 按块重排基。
      ok = bake_frame(f, /*record=*/true);
    }
    if (ok) {
      std::vector<EncodedPacket> flushed;
      chunk_encoder->Flush(&flushed);
      for (const auto& pkt : flushed) {
        chunk_muxer->WritePacket(0, pkt);
        ++encoded;
      }
      chunk_muxer->Finish();
      chunk_paths.push_back(chunk_out);
    } else {
      layer.Stop();
      return 1;
    }
  }
  layer.Stop();

  // 元数据导出（MC render_demo 对齐）：<out>.html_elements.json——
  // 元素级时序/矩形元数据，供合成工具与调试消费。
  {
    HtmlElementMetaDoc doc;
    doc.html_url = args.url;
    doc.mode = "bake";
    doc.bake_clock = args.clock;
    doc.view_width = drawable_w;
    doc.view_height = drawable_h;
    const auto table = layer.SnapshotElements();
    doc.report.collect_requested = true;
    doc.report.any_report = table.last_t_ms() >= 0;
    doc.report.entries = table.entries();
    doc.report.last_report_t_ms = table.last_t_ms();
    const std::string meta = BuildElementsMetaJson(doc);
    const std::string meta_path = HtmlElementsMetaJsonPath(args.out);
    std::FILE* mf = std::fopen(meta_path.c_str(), "wb");
    if (mf != nullptr) {
      std::fwrite(meta.data(), 1, meta.size(), mf);
      std::fclose(mf);
      std::printf("html_bake: metadata %s\n", meta_path.c_str());
    } else {
      std::fprintf(stderr, "html_bake: cannot write %s\n",
                   meta_path.c_str());
    }
  }

  if (chunk_paths.size() > 1) {
    ConcatParams cp;
    cp.inputs = chunk_paths;
    cp.output_path = args.out;
    const Status cst = Concat(cp);
    if (!cst) {
      std::fprintf(stderr, "html_bake: concat failed: %s\n",
                   cst.error().ToString().c_str());
      return 1;
    }
    for (const auto& path : chunk_paths) {
      std::remove(path.c_str());
    }
  }

  std::printf("html_bake: %s — %d frames, %lld packets, %dx%d @ %d fps "
              "(VT bake)\n",
              args.out.c_str(), total_frames, static_cast<long long>(encoded),
              drawable_w, drawable_h, args.fps);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

#endif  // XMEDIA_HAS_CEF
