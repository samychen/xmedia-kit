// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_
#define XMEDIA_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_

namespace xmedia::base {

template <typename Sig>
class OnceCallback;
template <typename Sig>
class RepeatingCallback;

using OnceClosure = OnceCallback<void()>;
using RepeatingClosure = RepeatingCallback<void()>;

}  // namespace xmedia::base

#endif  // XMEDIA_BASE_FUNCTIONAL_CALLBACK_FORWARD_H_
