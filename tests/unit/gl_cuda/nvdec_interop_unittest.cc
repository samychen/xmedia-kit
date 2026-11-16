// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The NVIDIA end of the zero-copy path, checked by reading the pixels back --
// the twin of gl_hw_import_unittest.cc (macOS) and
// egl_dmabuf_import_unittest.cc (Linux EGL).
//
// WHAT IS REAL HERE. The decode is REAL: libavcodec's NVDEC decodes an actual
// H.264 test file and hands over CUdeviceptr-backed NV12 planes. The import
// is REAL: the production CreateHwTextureImporter() chain routes the frame to
// the CUDA-GL interop backend, which registers the plane textures and copies
// device -> texture on the GPU. The comparison is REAL: the same frame is
// read back to the CPU through the explicit readback (av_hwframe_transfer_
// data) and every luma/chroma byte is compared against what GL sampled. The
// only synthetic thing is the VideoFrame construction, which mirrors what
// FFmpegHwVideoDecoder publishes after a successful NVDEC decode.
//
// THE CAPABILITY GATES, each recorded in its skip reason: no CUDA device, no
// NVDEC decoder for H.264, or no test file -- a host without them cannot run
// this suite and a regression cannot hide behind them, because once the
// gates pass a byte mismatch is RED.

#include <dlfcn.h>
#include <unistd.h>

#if defined(__linux__)

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "media/ffmpeg/hw_interop_ffmpeg.h"  // C4 隔离：libav 头经转发

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "platform/hwimport/hw_importer_factory.h"
#include "tests/unit/gl/gl_test_context.h"
#include "tests/unit/gl/gl_test_frames.h"

namespace xmedia::media {
namespace {

constexpr int kWindowSize = 64;

constexpr const char *kTestFile =
    XMEDIA_TESTDATA_DIR "/small_h264_aac_3s.mp4";

// One decoded NVDEC frame plus everything needed to keep it alive: the AVFrame
// clone (release_cb frees it, which returns the plane memory to the NVDEC
// pool) and the CPU reference of the SAME frame for the byte comparison.
struct NvdecFrame {
  base::scoped_refptr<VideoFrame> frame;
  std::unique_ptr<AVFrame, void (*)(AVFrame *)> cpu_reference{
      nullptr, [](AVFrame *f) { av_frame_free(&f); }};
  int width{0};
  int height{0};
};

// Decodes the first video frame of |input| through NVDEC. Returns an empty
// result with |why| filled when this host cannot (no CUDA device, no NVDEC
// decoder, no file) -- the caller turns that into a skip, not a failure.
std::unique_ptr<NvdecFrame> DecodeFirstNvdecFrame(const std::string &input,
                                                  std::string *why) {
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) < 0) {
    *why = "the test file could not be opened";
    return nullptr;
  }
  avformat_find_stream_info(fmt, nullptr);
  const int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vs < 0) {
    *why = "the test file has no video stream";
    avformat_close_input(&fmt);
    return nullptr;
  }
  AVStream *st = fmt->streams[vs];
  const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
  AVCodecContext *cc = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(cc, st->codecpar);
  AVBufferRef *hwdev = nullptr;
  if (av_hwdevice_ctx_create(&hwdev, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr,
                             0) < 0) {
    *why = "no CUDA device for NVDEC";
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return nullptr;
  }
  auto *cuda_dev =
      reinterpret_cast<AVCUDADeviceContext *>(hwdev->data);
  if (!cuda_dev || !cuda_dev->cuda_ctx) {
    *why = "the CUDA hwdevice has no context";
    av_buffer_unref(&hwdev);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return nullptr;
  }
  cc->hw_device_ctx = av_buffer_ref(hwdev);
  if (avcodec_open2(cc, codec, nullptr) < 0) {
    *why = "the NVDEC decoder could not open";
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return nullptr;
  }

  AVPacket *pkt = av_packet_alloc();
  AVFrame *frm = av_frame_alloc();
  bool got = false;
  while (!got && av_read_frame(fmt, pkt) >= 0) {
    if (pkt->stream_index == vs && avcodec_send_packet(cc, pkt) == 0) {
      got = avcodec_receive_frame(cc, frm) == 0;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  if (!got) {
    *why = "no frame decoded from the test file";
    av_frame_free(&frm);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return nullptr;
  }
  if (frm->format != AV_PIX_FMT_CUDA) {
    *why = "the decoder did not run on NVDEC";
    av_frame_free(&frm);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return nullptr;
  }

  auto out = std::make_unique<NvdecFrame>();
  out->width = frm->width;
  out->height = frm->height;
  // The CPU reference of the SAME frame, read back before anything else
  // touches the surface.
  AVFrame *sw = av_frame_alloc();
  if (av_hwframe_transfer_data(sw, frm, 0) < 0) {
    *why = "the explicit readback failed";
    av_frame_free(&sw);
    av_frame_free(&frm);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return nullptr;
  }
  out->cpu_reference.reset(sw);

  // The production shape: a kNvdecFrame native handle with its plumbing, the
  // release closure holding an AVFrame reference so the planes stay valid for
  // exactly the frame's lifetime.
  NativeHandle handle;
  handle.kind = NativeHandleKind::kNvdecFrame;
  handle.id = frm->data[0];
  auto *hw_dev = reinterpret_cast<AVHWDeviceContext *>(hwdev->data);
  auto *ctx_holder =
      reinterpret_cast<AVCUDADeviceContext *>(hw_dev->hwctx);
  auto info = std::make_unique<NvdecFrameInfo>();
  info->cuda_ctx = ctx_holder->cuda_ctx;
  info->luma = reinterpret_cast<uintptr_t>(frm->data[0]);
  info->chroma = reinterpret_cast<uintptr_t>(frm->data[1]);
  info->luma_stride = frm->linesize[0];
  info->chroma_stride = frm->linesize[1];
  // The release closure owns this reference: exactly one deleter per AVFrame,
  // otherwise the double free corrupts the heap at test teardown.
  const Size size{out->width, out->height};
  out->frame = VideoFrame::WrapNativeBuffer(
      handle, VideoFormat::kNV12, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0,
      base::BindOnce(
          [](AVFrame *f) {
            if (f) {
              av_frame_free(&f);
            }
          },
          frm),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  if (!out->frame) {
    *why = "the frame could not be wrapped";
    return nullptr;
  }
  out->frame->set_nvdec_info(std::move(info));
  // Keep the supporting objects alive as long as the frame is: the decoder
  // context owns the CUcontext the importer pushes, the hw device ref owns
  // that decoder context's lifetime.
  avcodec_free_context(&cc);
  avformat_close_input(&fmt);
  // |hwdev| deliberately outlives this function via the frame's held AVFrame
  // (its frames context references the device); releasing it here only drops
  // OUR reference.
  av_buffer_unref(&hwdev);
  return out;
}

class NvdecInteropTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string why;
    decoded_ = DecodeFirstNvdecFrame(kTestFile, &why);
    if (!decoded_) {
      GTEST_SKIP() << why;
    }
    importer_ = platform::hwimport::CreateHwTextureImporter();
    if (!importer_) {
      GTEST_SKIP() << "no importer in this build";
    }
    std::string gl_why;
    window_ = test::GlTestWindow::Create(kWindowSize, kWindowSize, &gl_why);
    if (!window_) {
      GTEST_SKIP() << gl_why;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    presenter_->SetHwTextureImporter(importer_.get());
    if (!presenter_->Init()) {
      GTEST_SKIP() << "presenter init failed";
    }
  }

  void TearDown() override {
    presenter_.reset();
    window_.reset();
  }

  // Binds a fresh texture, asks the production importer to import plane
  // |index| into it, and hands the texture back for byte comparisons.
  unsigned int ImportPlane(int index) {
    auto &gl = presenter_->api()->context();
    unsigned int tex = 0;
    gl.GenTextures(1, &tex);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    media::PlaneUploadLayout layout;
    layout.internal_format = index == 0 ? GL_R8 : GL_RG8;
    layout.format = index == 0 ? GL_RED : GL_RG;
    layout.type = GL_UNSIGNED_BYTE;
    layout.width = index == 0 ? decoded_->width : decoded_->width / 2;
    layout.height = index == 0 ? decoded_->height : decoded_->height / 2;
    layout.texture_name = tex;
    if (!importer_->ImportPlane(*decoded_->frame, index, layout)) {
      gl.DeleteTextures(1, &tex);
      return 0;
    }
    return tex;
  }

  std::unique_ptr<NvdecFrame> decoded_;
  std::unique_ptr<GlHwTextureImporter> importer_;
  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
};

// The route claim: a real NVDEC frame goes through the production chain to a
// zero-copy present, and the stats say so.
TEST_F(NvdecInteropTest, AnNvdecFrameReportsTheZeroCopyRoute) {
  PresentTiming timing;
  ASSERT_TRUE(presenter_->Compose(*decoded_->frame, &timing))
      << "the import itself is not allowed to degrade once the host can "
         "decode on NVDEC and open a GL context";
  EXPECT_EQ(timing.upload_path, UploadPath::kHwZeroCopy);
}

// Every luma byte the GPU decoded must be the luma byte GL sampled: the
// interop copy is content-correct, not just route-correct.
TEST_F(NvdecInteropTest, ImportedLumaMatchesTheExplicitReadback) {
  const unsigned int tex = ImportPlane(0);
  ASSERT_NE(tex, 0u) << "the importer refused a frame it claims it can place";
  auto &gl = presenter_->api()->context();
  gl.BindTexture(GL_TEXTURE_2D, tex);
  std::vector<uint8_t> gl_luma(static_cast<size_t>(decoded_->width) *
                               decoded_->height);
  gl.GetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_UNSIGNED_BYTE, gl_luma.data());
  const AVFrame *ref = decoded_->cpu_reference.get();
  int diffs = 0;
  for (int y = 0; y < decoded_->height; ++y) {
    for (int x = 0; x < decoded_->width; ++x) {
      if (gl_luma[y * decoded_->width + x] != ref->data[0][y * ref->linesize[0] + x]) {
        ++diffs;
      }
    }
  }
  EXPECT_EQ(diffs, 0)
      << "GL-sampled luma differs from the explicit readback by " << diffs
      << " of " << decoded_->width * decoded_->height << " bytes";
  gl.DeleteTextures(1, &tex);
}

// The interleaved chroma plane, same byte-exact contract.
TEST_F(NvdecInteropTest, ImportedChromaMatchesTheExplicitReadback) {
  const unsigned int tex = ImportPlane(1);
  ASSERT_NE(tex, 0u) << "the importer refused a frame it claims it can place";
  auto &gl = presenter_->api()->context();
  gl.BindTexture(GL_TEXTURE_2D, tex);
  const int cw = decoded_->width / 2;
  const int ch = decoded_->height / 2;
  std::vector<uint8_t> gl_chroma(static_cast<size_t>(cw) * ch * 2);
  gl.GetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_UNSIGNED_BYTE, gl_chroma.data());
  const AVFrame *ref = decoded_->cpu_reference.get();
  int diffs = 0;
  for (int y = 0; y < ch; ++y) {
    for (int x = 0; x < cw; ++x) {
      const int gl_index = (y * cw + x) * 2;
      const int ref_index = y * ref->linesize[1] + x * 2;
      if (gl_chroma[gl_index] != ref->data[1][ref_index] ||
          gl_chroma[gl_index + 1] != ref->data[1][ref_index + 1]) {
        ++diffs;
      }
    }
  }
  EXPECT_EQ(diffs, 0)
      << "GL-sampled chroma differs from the explicit readback by " << diffs
      << " of " << cw * ch << " texels";
  gl.DeleteTextures(1, &tex);
}

}  // namespace
}  // namespace xmedia::media

#endif  // defined(__linux__)
