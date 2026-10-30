// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/time/default_tick_clock.h"

namespace xmedia::base {

const DefaultTickClock* DefaultTickClock::GetInstance() {
  static const DefaultTickClock instance;
  return &instance;
}

}  // namespace xmedia::base
