// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// H7: the transcode video timeline. The old encoder set time_base from the
// fps DENOMINATOR ({1,1} for every integer-rate source) and fed raw
// microseconds as pts, so a 30 fps source came out with a timeline scaled by
// ~10^6; the frame converter hardcoded 1/90000 for every container. Both are
// asserted here against values no wrong unit can satisfy.

#include "media/transcode/ffmpeg_video_encoder.h"
#include "media/transcode/ffmpeg_transcode_streams.h"

#include <cstring>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "media/base/encoded_packet.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media {
namespace ff = ffmpeg;
namespace {

// One frame interval of a 30 fps source, in microseconds.
constexpr int64_t kFrameIntervalUs = 33333;

base::scoped_refptr<VideoFrame> MakeGrayFrame(base::TimeDelta ts) {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kI420, Size{16, 16}, Size{16, 16}, Rational{30, 1}, ts,
      base::Microseconds(kFrameIntervalUs), 0);
  return frame;
}

TEST(VideoEncoderTimeBaseTest, TimeBaseIsTheInverseFrameRate) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30;
  params.fps_den = 1;
  ASSERT_TRUE(encoder.Initialize(params));
  const AVRational tb = encoder.time_base();
  EXPECT_EQ(tb.num, 1);
  EXPECT_EQ(tb.den, 30)
      << "time_base must be the inverse frame rate, not {1, fps_den}";
}

// Uniform 30 fps input must come out as one tick per frame in the encoder's
// {1,30} time_base: pts 0, 1, 2, ... The old unit mismatch produced
// pts 0, 33333, 66666, ... -- a timeline where every frame lasted ~9 hours.
TEST(VideoEncoderTimeBaseTest, UniformFpsInputProducesConsecutivePts) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30;
  params.fps_den = 1;
  ASSERT_TRUE(encoder.Initialize(params));

  std::vector<EncodedPacket> out;
  for (int i = 0; i < 8; ++i) {
    std::vector<EncodedPacket> packets;
    ASSERT_TRUE(encoder.Encode(MakeGrayFrame(base::Microseconds(
                                   static_cast<int64_t>(i) * kFrameIntervalUs)),
                               &packets));
    out.insert(out.end(), std::make_move_iterator(packets.begin()),
               std::make_move_iterator(packets.end()));
  }
  std::vector<EncodedPacket> flushed;
  ASSERT_TRUE(encoder.Flush(&flushed));
  out.insert(out.end(), std::make_move_iterator(flushed.begin()),
             std::make_move_iterator(flushed.end()));

  // mjpeg has no B-frames and no encoder-side delay: every input frame yields
  // exactly one packet immediately, with pts == dts.
  ASSERT_EQ(out.size(), 8u);
  for (size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i].pts, static_cast<int64_t>(i))
        << "packet " << i << " pts is not one tick per frame interval";
    EXPECT_EQ(out[i].dts, out[i].pts);
    if (i > 0) {
      EXPECT_EQ(out[i].pts - out[i - 1].pts, 1);
    }
  }
}

// A non-integer rate exercises the rescale instead of the degenerate {1,1}:
// 29.97 fps (30000/1001) -> one input frame interval maps to 1001/30000 s,
// so consecutive frames differ by 1001 ticks of a {1001, 30000} time_base.
TEST(VideoEncoderTimeBaseTest, NonIntegerRateStaysOnTheTimeline) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30000;
  params.fps_den = 1001;
  ASSERT_TRUE(encoder.Initialize(params));
  const AVRational tb = encoder.time_base();
  ASSERT_EQ(tb.num, 1001);
  ASSERT_EQ(tb.den, 30000);

  std::vector<EncodedPacket> out;
  for (int i = 0; i < 4; ++i) {
    std::vector<EncodedPacket> packets;
    // Microsecond timestamps of the ideal 30000/1001 grid.
    const int64_t us = static_cast<int64_t>(i) * 1001 * 1000000 / 30000;
    ASSERT_TRUE(
        encoder.Encode(MakeGrayFrame(base::Microseconds(us)), &packets));
    out.insert(out.end(), std::make_move_iterator(packets.begin()),
               std::make_move_iterator(packets.end()));
  }
  ASSERT_EQ(out.size(), 4u);
  for (size_t i = 0; i < out.size(); ++i) {
    // With time_base {1001, 30000} one tick IS one frame interval
    // (1001/30000 s), so the ideal grid lands on tick i for any fps.
    EXPECT_EQ(out[i].pts, static_cast<int64_t>(i))
        << "packet " << i << " drifted off the frame grid";
  }
}

// The decode-side converter must honour the pkt_timebase it is handed: a
// container with time_base 1/1000 and pts 5000 means 5 s, not the 55.6 ms
// the hardcoded /90000 produced.
TEST(AvFrameToVideoFrameTest, HonoursTheGivenTimeBase) {
  ff::FramePtr frame(av_frame_alloc());
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = 4;
  frame->height = 4;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 32), 0);
  memset(frame->data[0], 0x10, static_cast<size_t>(frame->linesize[0]) * 4);
  memset(frame->data[1], 0x80, static_cast<size_t>(frame->linesize[1]) * 2);
  memset(frame->data[2], 0x80, static_cast<size_t>(frame->linesize[2]) * 2);
  frame->pts = 5000;  // 5000 * (1/1000) s = 5 s.
  frame->duration = 33;

  auto vf = AvFrameToVideoFrame(frame.get(), 4, 4, AVRational{1, 1000});
  ASSERT_TRUE(vf);
  EXPECT_EQ(vf->timestamp(), base::Seconds(5));
  EXPECT_EQ(vf->duration(), base::Milliseconds(33));

  // The historical 1/90000 time_base still gives the same answer as before.
  frame->pts = 90000;
  auto vf2 = AvFrameToVideoFrame(frame.get(), 4, 4, AVRational{1, 90000});
  ASSERT_TRUE(vf2);
  EXPECT_EQ(vf2->timestamp(), base::Seconds(1));
}

// A decoder is not obliged to hand back 8-bit I420: a 10-bit source packs one
// sample into two bytes, so the width-byte-per-row copy this function used to
// do read only every other sample and left the rest of the picture black.
// The transcode of a real 10-bit file looked "successful" (muxer OK, file
// re-opens, right duration) while the picture was a bright banded mess --
// SSIM 0.035 against the source. This test pins the pixel VALUES, which is
// the only way to see it: geometry and timestamps were never wrong.
TEST(AvFrameToVideoFrameTest, ConvertsTenBitSourceToEightBitI420) {
  ff::FramePtr frame(av_frame_alloc());
  frame->format = AV_PIX_FMT_YUV420P10LE;
  frame->width = 4;
  frame->height = 4;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 32), 0);
  // 10-bit white in the Y plane (1023) and neutral chroma (512). A 16-bit
  // little-endian sample 0x03FF is bytes FF 03; taking only "width" bytes per
  // row kept the FF and dropped the 03, producing 0xFF = 255 in 8-bit --
  // which HAPPENS to look white. So the assertion that discriminates is not
  // "is it white" but "is the mid-grey plane mid-grey": the old code read
  // chroma at half the real stride and produced 0xFF there too.
  auto* y = reinterpret_cast<uint16_t*>(frame->data[0]);
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      y[(static_cast<size_t>(r) * static_cast<size_t>(frame->linesize[0]) / 2) +
        static_cast<size_t>(c)] = 1023;
    }
  }
  for (int p = 1; p <= 2; ++p) {
    auto* plane = reinterpret_cast<uint16_t*>(frame->data[p]);
    for (int r = 0; r < 2; ++r) {
      for (int c = 0; c < 2; ++c) {
        plane[(static_cast<size_t>(r) *
               static_cast<size_t>(frame->linesize[p]) / 2) +
              static_cast<size_t>(c)] = 512;
      }
    }
  }
  frame->pts = 0;
  frame->duration = 1;

  auto vf = AvFrameToVideoFrame(frame.get(), 4, 4, AVRational{1, 10});
  ASSERT_TRUE(vf);
  // 10-bit 1023 -> 8-bit 255 (limited range is stretched by the converter's
  // sws path; the point is that it is bright, not that it is exact).
  EXPECT_GE(vf->visible_data(VideoFrame::kYPlane)[0], 235);
  // Neutral chroma 512 must land near 128. The old copy produced 255 here,
  // i.e. a violently saturated picture, which is exactly what the real
  // 10-bit transcode showed.
  const int u = vf->visible_data(VideoFrame::kUPlane)[0];
  const int v = vf->visible_data(VideoFrame::kVPlane)[0];
  EXPECT_NEAR(u, 128, 24) << "U plane came out " << u;
  EXPECT_NEAR(v, 128, 24) << "V plane came out " << v;
}

// NV12 has ONE interleaved chroma plane: data[2] is null. The old code read
// data[2] unconditionally, so the V plane was whatever the null-safe memcpy
// left behind -- a colour-shifted picture that still had the right size and
// the right timestamps.
TEST(AvFrameToVideoFrameTest, ConvertsNv12SourceToEightBitI420) {
  ff::FramePtr frame(av_frame_alloc());
  frame->format = AV_PIX_FMT_NV12;
  frame->width = 4;
  frame->height = 4;
  ASSERT_EQ(av_frame_get_buffer(frame.get(), 32), 0);
  ASSERT_EQ(frame->data[2], nullptr)
      << "NV12 must have exactly two planes; the test premise is wrong";
  for (int r = 0; r < 4; ++r) {
    memset(frame->data[0] + static_cast<ptrdiff_t>(r) * frame->linesize[0], 200,
           4);
  }
  // Interleaved U,V pairs: every byte 128 is neutral.
  for (int r = 0; r < 2; ++r) {
    memset(frame->data[1] + static_cast<ptrdiff_t>(r) * frame->linesize[1], 128,
           4);
  }
  frame->pts = 0;
  frame->duration = 1;

  auto vf = AvFrameToVideoFrame(frame.get(), 4, 4, AVRational{1, 10});
  ASSERT_TRUE(vf);
  EXPECT_NEAR(vf->visible_data(VideoFrame::kYPlane)[0], 200, 8);
  EXPECT_NEAR(vf->visible_data(VideoFrame::kUPlane)[0], 128, 8);
  EXPECT_NEAR(vf->visible_data(VideoFrame::kVPlane)[0], 128, 8);
}

// T9a static-frame dedup: consecutive frames with identical content must
// skip the conversion pipeline (counted by dedup_skipped_frames) while the
// output timeline stays intact — a dedup'd frame is the cached conversion
// result re-sent at the new pts, so packet pts must still advance by one
// tick per frame. A content change must reset the skip.
TEST(VideoEncoderDedupTest, IdenticalConsecutiveFramesSkipConversion) {
  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "mjpeg";
  params.width = 16;
  params.height = 16;
  params.fps_num = 30;
  params.fps_den = 1;
  ASSERT_TRUE(encoder.Initialize(params));

  std::vector<EncodedPacket> out;
  // 5 identical frames (MakeGrayFrame is constant black) at consecutive
  // timestamps, then one bright frame (content change).
  for (int i = 0; i < 5; ++i) {
    std::vector<EncodedPacket> packets;
    ASSERT_TRUE(encoder.Encode(MakeGrayFrame(base::Microseconds(
                                   static_cast<int64_t>(i) * kFrameIntervalUs)),
                               &packets));
    out.insert(out.end(), std::make_move_iterator(packets.begin()),
               std::make_move_iterator(packets.end()));
  }
  EXPECT_EQ(encoder.dedup_skipped_frames(), 4)
      << "frames 1..4 must dedup against frame 0";

  auto changed = MakeGrayFrame(base::Microseconds(5 * kFrameIntervalUs));
  auto* y = changed->visible_data(VideoFrame::kYPlane).data();
  std::memset(const_cast<uint8_t*>(y), 0xEB,
              changed->visible_data(VideoFrame::kYPlane).size());
  std::vector<EncodedPacket> packets;
  ASSERT_TRUE(encoder.Encode(std::move(changed), &packets));
  out.insert(out.end(), std::make_move_iterator(packets.begin()),
             std::make_move_iterator(packets.end()));

  EXPECT_EQ(encoder.dedup_skipped_frames(), 4)
      << "the content change must not dedup";

  std::vector<EncodedPacket> flushed;
  ASSERT_TRUE(encoder.Flush(&flushed));
  out.insert(out.end(), std::make_move_iterator(flushed.begin()),
             std::make_move_iterator(flushed.end()));

  // The timeline must be untouched by dedup: one packet per frame with pts
  // advancing one tick per frame interval (mjpeg: no delay, no B-frames).
  ASSERT_EQ(out.size(), 6u);
  for (size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i].pts, static_cast<int64_t>(i))
        << "packet " << i << " pts broke the frame grid";
  }
}


// VP9 alpha（HyperFrames alpha_mode=1 对齐）：kARGB 输入经 BGRA→YUVA420P
// 转换、libvpx-vp9 alpha_mode=1 编进同一条 VP9 流，解码回来必须是
// yuva420p 且 A 面保留每帧的透明度。判别性断言在 alpha 平面：不透明帧
// A=255、透明帧 A=0——没有 alpha 通路时 sws 的 A 面是未定义字节，解回
// 来不会稳定为这两个值。
TEST(VideoEncoderAlphaTest, Vp9AlphaRoundTripsThroughYuva420p) {
  const AVCodec* enc = avcodec_find_encoder_by_name("libvpx-vp9");
  if (enc == nullptr) {
    GTEST_SKIP() << "libvpx-vp9 not in this FFmpeg build";
  }
  // 原生 vp9 解码器不读 alpha 层（FFmpeg 7.1.1 的 vp9.c 无 alpha 路径）；
  // alpha 解码只走 libvpx-vp9 解码器（libvpxdec 从 MATROSKA_BLOCKADDITIONAL
  // side data 取 alpha 流）。
  const AVCodec* dec = avcodec_find_decoder_by_name("libvpx-vp9");
  if (dec == nullptr) {
    GTEST_SKIP() << "libvpx-vp9 decoder not in this FFmpeg build";
  }

  FFmpegVideoEncoder encoder;
  FFmpegVideoEncoder::Params params;
  params.codec_name = "libvpx-vp9";
  params.width = 32;
  params.height = 32;
  params.fps_num = 30;
  params.fps_den = 1;
  params.options["alpha_mode"] = "1";
  ASSERT_TRUE(encoder.Initialize(params));

  // 两帧 kARGB：帧 0 不透明红（A=255），帧 1 全透明绿（A=0）。kARGB 的
  // 内存序是 B,G,R,A。
  auto make_argb = [&](uint8_t a, uint8_t r, uint8_t g, uint8_t b,
                       int64_t ms) {
    auto frame = VideoFrame::CreateBlackFrame(
        VideoFormat::kARGB, Size{32, 32}, Size{32, 32}, Rational{1, 1},
        base::Milliseconds(ms), base::Milliseconds(33), 0);
    const auto data = frame->mutable_data(VideoFrame::kYPlane);
    const int32_t stride = frame->stride(VideoFrame::kYPlane);
    for (int y = 0; y < 32; ++y) {
      for (int x = 0; x < 32; ++x) {
        uint8_t* px =
            data.data() + static_cast<size_t>(y) * stride + x * 4;
        px[0] = b;
        px[1] = g;
        px[2] = r;
        px[3] = a;
      }
    }
    return frame;
  };

  std::vector<EncodedPacket> packets;
  ASSERT_TRUE(encoder.Encode(make_argb(255, 255, 0, 0, 0), &packets));
  ASSERT_TRUE(encoder.Encode(make_argb(0, 0, 255, 0, 33), &packets));
  ASSERT_TRUE(encoder.Flush(&packets));
  ASSERT_FALSE(packets.empty()) << "vp9 must emit packets";

  // 直接解码（不进容器）：VP9 的 alpha 在码流里，容器无关。
  AVCodecContext* dctx = avcodec_alloc_context3(dec);
  ASSERT_NE(dctx, nullptr);
  dctx->pkt_timebase = AVRational{1, 30};
  ASSERT_EQ(avcodec_open2(dctx, dec, nullptr), 0);

  std::vector<AVFrame*> frames;
  for (auto& pkt : packets) {
    // av_new_packet + memcpy，不用 av_packet_from_data：后者不拷贝，会把
    // vector 的 new[] 内存挂上 av_buffer_default_free——release 时以
    // av_free 释放 new[] 内存是 UB，堆损坏会在后续测试里随机 SIGABRT。
    AVPacket* p = av_packet_alloc();
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(av_new_packet(p, static_cast<int>(pkt.data.size())), 0);
    memcpy(p->data, pkt.data.data(), pkt.data.size());
    p->pts = pkt.pts;
    p->dts = pkt.dts;
    // VP9 alpha 不在码流字节里：FFmpeg 7.x 把 alpha 流放在
    // AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL side data 上，解码器从这里取。
    for (const auto& sd : pkt.side_data) {
      uint8_t* buf = av_packet_new_side_data(
          p, static_cast<AVPacketSideDataType>(sd.type),
          static_cast<int>(sd.bytes.size()));
      ASSERT_NE(buf, nullptr) << "side data " << sd.type;
      memcpy(buf, sd.bytes.data(), sd.bytes.size());
    }
    ASSERT_EQ(avcodec_send_packet(dctx, p), 0);
    av_packet_free(&p);
    AVFrame* f = av_frame_alloc();
    while (avcodec_receive_frame(dctx, f) >= 0) {
      frames.push_back(av_frame_clone(f));
      av_frame_unref(f);
    }
    av_frame_free(&f);
  }
  // Encoder delay: drain with a null packet.
  ASSERT_EQ(avcodec_send_packet(dctx, nullptr), 0);
  AVFrame* f = av_frame_alloc();
  while (avcodec_receive_frame(dctx, f) >= 0) {
    frames.push_back(av_frame_clone(f));
    av_frame_unref(f);
  }
  av_frame_free(&f);

  ASSERT_EQ(frames.size(), 2u) << "one decoded frame per input frame";
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(frames[i]->format, AV_PIX_FMT_YUVA420P)
        << "frame " << i << " must decode with its alpha plane";
    ASSERT_NE(frames[i]->data[3], nullptr);
    const int expected_alpha = (i == 0) ? 255 : 0;
    EXPECT_NEAR(frames[i]->data[3][0], expected_alpha, 8)
        << "frame " << i << " alpha plane corner";
    EXPECT_NEAR(frames[i]->data[3][static_cast<size_t>(frames[i]->linesize[3]) * 16 + 16],
                expected_alpha, 8)
        << "frame " << i << " alpha plane centre";
    av_frame_free(&frames[i]);
  }
  avcodec_free_context(&dctx);
}

}  // namespace
}  // namespace xmedia::media
