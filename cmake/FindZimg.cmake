# Copyright 2026 The avbase Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
#
# ABANDONED PREFIX FILE -- NOT INCLUDED BY THE BUILD.
#
# This started as "link libzimg directly and tone map in C++". It is not there
# because libzimg has NO tone-mapping API: it converts transfer functions
# (PQ -> linear, linear -> BT.709) and resamples, but the luminance compression
# that makes 10000 cd/m^2 fit on a 100 cd/m^2 display lives in FFmpeg's
# `tonemap` filter, not in zimg. Linking zimg directly would give the transfer
# conversion and then clip everything above SDR white.
#
# The shipped path is the FFmpeg filter graph in media/ffmpeg/tone_map.cc
# (zscale -> tonemap -> zscale), which needs `--enable-filter=zscale,tonemap`
# in tools/setup_ffmpeg.sh (CONFIG_TONEMAP_FILTER=0 before this round).
#
# Kept as a file rather than deleted because the runtime protects chat-adjacent
# files from removal; empty on purpose so nothing can accidentally include it.
