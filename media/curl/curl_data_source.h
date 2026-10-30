// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_CURL_CURL_DATA_SOURCE_H_
#define XMEDIA_MEDIA_CURL_CURL_DATA_SOURCE_H_

#include <stdint.h>

#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "base/task/task_runner.h"
#include "base/time/time.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/curl/media_curl_export.h"
#include "media/filters/async_data_source_base.h"

namespace xmedia::media::curl {

// A libcurl-backed DataSource for http(s) URIs -- the port of CicadaPlayer's
// curl_data_source to xmedia-kit's DataSource contract. It exists because the
// avio-backed UrlDataSource cannot express the network controls a CDN-backed
// playback session needs: pinned DNS answers, forced IPv4/IPv6, per-request
// headers, a proxy, and a low-speed watchdog that kills a stalled transfer
// instead of blocking forever. Everything else (redirects, TLS) curl does.
//
// READ MODEL. Every ReadBlocking(offset, size) issues `Range:
// bytes=offset-(offset+size-1)` on ONE long-lived easy handle, so curl's
// connection cache gives HTTP keep-alive (and TLS session reuse) across both
// sequential reads and seeks: a seek is one request on a warm connection,
// not a reconnect. The bounded range also makes each transfer end exactly
// when the caller's buffer is full, which is what keeps the connection
// reusable. A server that answers 200 to a Range request (i.e. ignores it)
// is reported as not seekable; a jump on such a connection fails with
// kSourceSeekFailed instead of silently returning bytes from the wrong
// offset. A 416 answer (offset at/past the end) is EOF.
//
// THREADING/LIFETIME: ReadBlocking runs on the demux thread and may block
// for the network duration; Abort() flips the flag the transfer-progress
// callback polls, so a stalled read returns kAborted promptly (Delta 15).
// The async Read() runs on the shared AsyncDataSourceBase worker and posts
// its result to |task_runner|, never inline.
class XMEDIA_CURL_EXPORT CurlDataSource final : public DataSource {
 public:
  enum class IpResolve {
    kUnspec,  // let the resolver decide (default)
    kV4,      // force A records (CURLOPT_IPRESOLVE V4)
    kV6,      // force AAAA records (CURLOPT_IPRESOLVE V6)
  };

  struct Config {
    base::TimeDelta connect_timeout{base::Seconds(15)};
    // Low-speed watchdog: abort the transfer when it stays below
    // |low_speed_limit| bytes/s for |low_speed_time|. Mirrors CicadaPlayer's
    // stalled-transfer protection; <= 0 disables.
    int64_t low_speed_limit{1024};
    base::TimeDelta low_speed_time{base::Seconds(30)};
    std::string user_agent;
    std::string referer;
    std::string proxy;
    // Sent verbatim on every request (CicadaPlayer's AddCustomHttpHeader).
    std::map<std::string, std::string> headers;
    // Custom IP resolution (CicadaPlayer's customer-ip-resolve): host ->
    // "port:address", sent as a CURLOPT_RESOLVE entry "host:port:address".
    // The resolver is skipped entirely for pinned hosts.
    std::map<std::string, std::string> dns_overrides;
    IpResolve ip_resolve{IpResolve::kUnspec};
    bool follow_redirects{true};
    int max_redirects{10};
  };

  // True for the schemes this source implements. The player wiring uses it
  // to pick between this and the avio-backed UrlDataSource.
  static bool SupportsUri(std::string_view uri);

  CurlDataSource(std::string uri, Config config);
  CurlDataSource(const CurlDataSource&) = delete;
  CurlDataSource& operator=(const CurlDataSource&) = delete;
  ~CurlDataSource() override;

  // ---- DataSource ----------------------------------------------------------
  void SetHost(Host* host) override;
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            ReadCB read_cb) override;
  ReadResult ReadBlocking(int64_t offset, size_t size, uint8_t* data) override;
  void Abort() override;
  bool GetSize(int64_t* size_out) override;
  bool IsStreaming() const override;
  void SetBitrate(int bitrate) override;
  bool IsSeekable() const override;

 private:
  // libcurl callbacks. They are private static members (not file-local free
  // functions) so the header/progress state they need stays encapsulated.
  // The transfer-progress (abort) thunk is a file-local function in the .cc:
  // its libcurl signature needs curl_off_t, and this header must stay
  // includable on hosts without libcurl (the player facade includes it
  // unconditionally).
  static size_t WriteThunk(char* ptr, size_t size, size_t nmemb,
                           void* userdata);
  static size_t HeaderThunk(char* ptr, size_t size, size_t nmemb,
                            void* userdata);
  // One header line from the current response, status line included.
  void OnHeaderLine(std::string_view line);

  // Makes sure an easy handle exists and a Range request for |offset| is
  // prepared. Requires |lock_|.
  ReadResult PrepareRequestLocked(int64_t offset, size_t size);
  // Performs the prepared transfer and returns the bytes received.
  ReadResult PerformLocked(int64_t offset, size_t size, uint8_t* data);
  // Maps a failed transfer to the structured MediaError contract
  // (summary / detail / actionable suggestion).
  MediaError HttpErrorLocked(int status, const std::string& curl_detail);
  MediaError CurlErrorLocked(int curl_code);
  void CloseLocked();

  const std::string uri_;
  const Config config_;
  mutable std::mutex lock_;
  void* easy_ = nullptr;  // CURL*, void* here to keep curl/curl.h out of the
                          // header (only the .cc pays for it).
  void* request_headers_ = nullptr;  // curl_slist* for this request
  void* resolve_list_ = nullptr;     // curl_slist* for CURLOPT_RESOLVE
  char error_buffer_[256] = {};      // CURL_ERROR_SIZE, owned by |easy_|
  // The offset the most recent transfer was issued for, -1 when none.
  int64_t active_request_offset_ = -1;
  int64_t size_ = -1;
  bool size_known_ = false;
  bool range_accepted_ = false;
  int header_status_code_ = 0;

  // Shared async-Read plumbing (worker, job queue, abort flag). Declared last
  // so the members above are fully constructed before its worker can run
  // ReadBlocking, mirroring RetryDataSource and UrlDataSource.
  AsyncDataSourceBase async_;
};

}  // namespace xmedia::media::curl

#endif  // XMEDIA_MEDIA_CURL_CURL_DATA_SOURCE_H_
