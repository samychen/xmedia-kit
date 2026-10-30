// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_BASE_KEY_PROVIDER_H_
#define XMEDIA_MEDIA_BASE_KEY_PROVIDER_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace xmedia::media {

// Where decryption keys come from -- the port of CicadaPlayer's
// DrmManager/DrmHandler seam, reduced to what xmedia-kit's demuxers actually
// consume today: "give me the bytes that unlock this URI". The HLS
// demuxer calls it for EXT-X-KEY (AES-128: 16 raw bytes); a CDM-backed
// product (WideVine, FairPlay) implements the same seam against its own
// machinery, which is the extension point CicadaPlayer's
// setDrmRequestCallback played and xmedia-kit's D8 decision left open.
//
// THREADING. GetKeyBlocking runs on the CALLER's thread -- the demuxer's
// worker -- and may block for the network duration; it must honour no
// contract beyond returning promptly after an external Stop() (the demuxer
// abandons stale results). GetKey is the async form for hosts that prefer
// callbacks; the demuxer today only uses the blocking form.
class XMEDIA_MEDIA_EXPORT KeyProvider
    : public base::RefCountedThreadSafe<KeyProvider> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  using GetKeyCB = base::OnceCallback<void(
      base::expected<std::vector<uint8_t>, MediaError>)>;

  KeyProvider(const KeyProvider&) = delete;
  KeyProvider& operator=(const KeyProvider&) = delete;

  // Blocking fetch of the key bytes for |key_uri| (the URI exactly as the
  // playlist carried it). A wrong-length or unavailable key is an error,
  // never empty bytes.
  virtual base::expected<std::vector<uint8_t>, MediaError>
  GetKeyBlocking(const std::string& key_uri) = 0;

  virtual void GetKey(const std::string& key_uri, GetKeyCB cb) {
    std::move(cb).Run(GetKeyBlocking(key_uri));
  }

 protected:
  friend class base::RefCountedThreadSafe<KeyProvider>;
  KeyProvider() = default;
  virtual ~KeyProvider() = default;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_BASE_KEY_PROVIDER_H_
