// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/curl/curl_data_source.h"

#include <curl/curl.h>

#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "media/base/media_error.h"

namespace xmedia::media::curl {
namespace {

std::once_flag& GlobalInitOnce() {
  static std::once_flag once;
  return once;
}

// The transfer-progress (abort) callback. It observes the shared abort flag
// through the owner's AsyncDataSourceBase, so a Stop()/Abort() from any
// thread ends an in-flight transfer promptly (the low-speed watchdog covers
// the nobody-is-calling-us case).
int ProgressAbortThunk(void* clientp, curl_off_t, curl_off_t, curl_off_t,
                       curl_off_t) {
  auto* async = static_cast<AsyncDataSourceBase*>(clientp);
  return async->IsAborted() ? 1 : 0;
}

// Per-transfer destination handed to the write callback.
struct TransferScratch {
  uint8_t* dst = nullptr;
  size_t capacity = 0;
  size_t received = 0;
};

// Parses a non-negative decimal, rejecting trailing junk.
bool ParseNonNegative(std::string_view value, int64_t* out) {
  if (value.empty()) {
    return false;
  }
  int64_t parsed = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }
  *out = parsed;
  return true;
}

// Lower-cases |line|'s first |n| characters for header-name matching.
bool HeaderIs(std::string_view line, std::string_view name) {
  if (line.size() < name.size()) {
    return false;
  }
  for (size_t i = 0; i < name.size(); ++i) {
    char c = line[i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != name[i]) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool CurlDataSource::SupportsUri(std::string_view uri) {
  return uri.starts_with("http://") || uri.starts_with("https://");
}

CurlDataSource::CurlDataSource(std::string uri, Config config)
    : uri_(std::move(uri)),
      config_(config),
      async_("xmedia-curl-src",
             base::BindRepeating(&CurlDataSource::ReadBlocking,
                                 base::Unretained(this))) {
  std::call_once(GlobalInitOnce(),
                 [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

CurlDataSource::~CurlDataSource() {
  async_.SetAborted();
  {
    std::lock_guard<std::mutex> scoped(lock_);
    CloseLocked();
  }
  async_.StopWorker();
}

void CurlDataSource::SetHost(Host* host) {
  // Nothing to defer: this source either has the bytes or fails; the
  // live-edge deferral contract belongs to LiveDataSource.
  (void)host;
}

void CurlDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                          base::scoped_refptr<base::TaskRunner> task_runner,
                          ReadCB read_cb) {
  async_.Read(offset, size, data, std::move(task_runner), std::move(read_cb));
}

void CurlDataSource::Abort() {
  async_.SetAborted();
}

bool CurlDataSource::GetSize(int64_t* size_out) {
  std::lock_guard<std::mutex> scoped(lock_);
  if (!size_known_) {
    return false;
  }
  if (size_out != nullptr) {
    *size_out = size_;
  }
  return true;
}

bool CurlDataSource::IsSeekable() const {
  std::lock_guard<std::mutex> scoped(lock_);
  // Only a server that honors Range can serve the arbitrary offsets the
  // DataSourceIO bridge asks for. Until a 206 (or an Accept-Ranges header)
  // proves it, assume not.
  return range_accepted_;
}

bool CurlDataSource::IsStreaming() const {
  std::lock_guard<std::mutex> scoped(lock_);
  // Live-like: no length advertised and no way to seek. A finite file on a
  // range-less server is not "streaming" -- it is just unseekable.
  return !size_known_ && !range_accepted_;
}

void CurlDataSource::SetBitrate(int bitrate) {
  // Informational for HTTP; curl has no use for the estimate.
  (void)bitrate;
}

size_t CurlDataSource::WriteThunk(char* ptr, size_t size, size_t nmemb,
                                  void* userdata) {
  auto* ctx = static_cast<TransferScratch*>(userdata);
  const size_t bytes = size * nmemb;
  const size_t room = ctx->capacity - ctx->received;
  const size_t copy = bytes < room ? bytes : room;
  if (copy > 0) {
    memcpy(ctx->dst + ctx->received, ptr, copy);
    ctx->received += copy;
  }
  // Returning a short count makes curl abort the transfer with
  // CURLE_WRITE_ERROR; PerformLocked distinguishes "caller buffer full" (a
  // complete partial read) from a real write failure by the received count.
  return copy;
}

size_t CurlDataSource::HeaderThunk(char* ptr, size_t size, size_t nmemb,
                                   void* userdata) {
  static_cast<CurlDataSource*>(userdata)->OnHeaderLine(
      std::string_view(ptr, size * nmemb));
  return size * nmemb;
}

void CurlDataSource::OnHeaderLine(std::string_view line) {
  // Runs inside curl_easy_perform, which ReadBlocking calls while holding
  // lock_ on the same thread -- taking lock_ here would self-deadlock. All
  // fields this writes are only read under lock_ from other threads, and the
  // window is bounded by the transfer itself.
  while (!line.empty() &&
         (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
    line.remove_suffix(1);
  }
  if (line.starts_with("HTTP/")) {
    // Status line: "HTTP/1.1 206 Partial Content". Reset per response.
    const size_t first_space = line.find(' ');
    header_status_code_ = 0;
    if (first_space != std::string_view::npos) {
      std::string_view code = line.substr(first_space + 1, 3);
      int64_t parsed = 0;
      if (ParseNonNegative(code, &parsed)) {
        header_status_code_ = static_cast<int>(parsed);
      }
    }
    return;
  }
  constexpr std::string_view kContentRange = "content-range:";
  constexpr std::string_view kAcceptRanges = "accept-ranges:";
  constexpr std::string_view kContentLength = "content-length:";
  if (HeaderIs(line, kContentRange)) {
    // "bytes 0-1023/40960" -- the total after the final '/' is the object
    // size. "bytes */40960" (a 416) carries the total the same way.
    const size_t slash = line.rfind('/');
    int64_t total = -1;
    if (slash != std::string_view::npos &&
        ParseNonNegative(line.substr(slash + 1), &total)) {
      size_ = total;
      size_known_ = true;
    }
    return;
  }
  if (HeaderIs(line, kAcceptRanges)) {
    if (line.substr(kAcceptRanges.size()).find("bytes") !=
        std::string_view::npos) {
      range_accepted_ = true;
    }
    return;
  }
  if (HeaderIs(line, kContentLength)) {
    // A 200 answer's Content-Length is the whole object; a 206's is only the
    // range length, so it must not overwrite the total (Content-Range above
    // already provided it).
    int64_t parsed = -1;
    std::string_view value = line.substr(kContentLength.size());
    if (header_status_code_ == 200 && ParseNonNegative(value, &parsed)) {
      size_ = parsed;
      size_known_ = true;
    }
  }
}

DataSource::ReadResult CurlDataSource::ReadBlocking(int64_t offset, size_t size,
                                                    uint8_t* data) {
  if (size == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> scoped(lock_);
  ReadResult prepared = PrepareRequestLocked(offset, size);
  if (!prepared.has_value()) {
    return prepared;
  }
  return PerformLocked(offset, size, data);
}

DataSource::ReadResult CurlDataSource::PrepareRequestLocked(int64_t offset,
                                                            size_t size) {
  // Every request here is a fresh bounded Range request; a 200 answer to it
  // means the server ignored the Range and the body starts at 0. Once that
  // has been observed, only offset 0 is servable -- anything else is an
  // honest seek failure rather than silently wrong bytes.
  if (easy_ != nullptr && !range_accepted_ && header_status_code_ == 200 &&
      offset != 0) {
    return base::unexpected(
        Err(ErrorCode::kSourceSeekFailed, "the server does not seek",
            "url = " + uri_ + ", requested offset = " + std::to_string(offset),
            "use a range-capable server, or play this uri linearly"));
  }
  if (easy_ == nullptr) {
    // One handle for the whole session: curl's connection cache inside it is
    // what makes every subsequent request reuse the TCP/TLS connection
    // (keep-alive). Recreating the handle per read would throw that away.
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
      return base::unexpected(
          Err(ErrorCode::kOutOfMemory, "curl handle allocation failed",
              "url = " + uri_,
              "free memory, or reduce concurrent player instances"));
    }
    easy_ = easy;
    error_buffer_[0] = '\0';
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION,
                     config_.follow_redirects ? 1L : 0L);
    curl_easy_setopt(easy, CURLOPT_MAXREDIRS,
                     static_cast<long>(config_.max_redirects));
    curl_easy_setopt(
        easy, CURLOPT_CONNECTTIMEOUT_MS,
        static_cast<long>(config_.connect_timeout.InMilliseconds()));
    if (config_.low_speed_limit > 0 &&
        config_.low_speed_time > base::TimeDelta()) {
      curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT,
                       static_cast<long>(config_.low_speed_limit));
      curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME,
                       static_cast<long>(config_.low_speed_time.InSeconds()));
    }
    curl_easy_setopt(
        easy, CURLOPT_IPRESOLVE,
        static_cast<long>(config_.ip_resolve == IpResolve::kV4
                              ? CURL_IPRESOLVE_V4
                              : (config_.ip_resolve == IpResolve::kV6
                                     ? CURL_IPRESOLVE_V6
                                     : CURL_IPRESOLVE_WHATEVER)));
    if (!config_.user_agent.empty()) {
      curl_easy_setopt(easy, CURLOPT_USERAGENT, config_.user_agent.c_str());
    }
    if (!config_.referer.empty()) {
      curl_easy_setopt(easy, CURLOPT_REFERER, config_.referer.c_str());
    }
    if (!config_.proxy.empty()) {
      curl_easy_setopt(easy, CURLOPT_PROXY, config_.proxy.c_str());
    }
    curl_slist* resolves = nullptr;
    for (const auto& [host, port_addr] : config_.dns_overrides) {
      resolves = curl_slist_append(resolves, (host + ":" + port_addr).c_str());
    }
    if (resolves != nullptr) {
      curl_easy_setopt(easy, CURLOPT_RESOLVE, resolves);
    }
    resolve_list_ = resolves;
    curl_easy_setopt(easy, CURLOPT_URL, uri_.c_str());
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, WriteThunk);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, HeaderThunk);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, ProgressAbortThunk);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &async_);
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_buffer_);
  }

  // Per-request state: a fresh bounded Range header list. The previous
  // request's list is dead by now (its perform completed), so freeing it
  // here is safe.
  if (request_headers_ != nullptr) {
    curl_slist_free_all(static_cast<curl_slist*>(request_headers_));
    request_headers_ = nullptr;
  }
  curl_slist* headers = nullptr;
  std::string range = "Range: bytes=" + std::to_string(offset) + "-" +
                      std::to_string(offset + static_cast<int64_t>(size) - 1);
  headers = curl_slist_append(headers, range.c_str());
  for (const auto& [name, value] : config_.headers) {
    headers = curl_slist_append(headers, (name + ": " + value).c_str());
  }
  if (headers != nullptr) {
    curl_easy_setopt(static_cast<CURL*>(easy_), CURLOPT_HTTPHEADER, headers);
  }
  request_headers_ = headers;

  active_request_offset_ = offset;
  header_status_code_ = 0;
  return 0;  // prepared; the value is unused, only has_value() is read
}

DataSource::ReadResult
CurlDataSource::PerformLocked(int64_t offset, size_t size, uint8_t* data) {
  TransferScratch ctx{data, size, 0};
  curl_easy_setopt(static_cast<CURL*>(easy_), CURLOPT_WRITEDATA, &ctx);
  const CURLcode code = curl_easy_perform(static_cast<CURL*>(easy_));

  if (async_.IsAborted()) {
    CloseLocked();
    return base::unexpected(
        Err(ErrorCode::kAborted, "the curl source was aborted",
            "url = " + uri_ + ", offset = " + std::to_string(offset),
            "this follows Stop(); no action needed"));
  }
  if (code == CURLE_WRITE_ERROR && ctx.received == ctx.capacity) {
    // Buffer-full write abort: the complete partial read the caller asked
    // for. The connection is dead to curl, but that only costs a new
    // request next time, on the still-cached connection.
    return static_cast<int>(ctx.received);
  }
  if (code != CURLE_OK) {
    MediaError error = CurlErrorLocked(static_cast<int>(code));
    CloseLocked();
    return base::unexpected(std::move(error));
  }

  long status = 0;
  curl_easy_getinfo(static_cast<CURL*>(easy_), CURLINFO_RESPONSE_CODE, &status);
  if (status == 416) {
    // Range not satisfiable == reading at/past the end.
    CloseLocked();
    return 0;
  }
  if (status >= 400) {
    MediaError error = HttpErrorLocked(static_cast<int>(status), "");
    CloseLocked();
    return base::unexpected(std::move(error));
  }
  if (status == 200 && offset > 0) {
    // The server ignored our Range: this body starts at 0, not |offset|.
    CloseLocked();
    return base::unexpected(
        Err(ErrorCode::kSourceSeekFailed, "the server does not seek",
            "url = " + uri_ + ", http = 200 for a range request at offset " +
                std::to_string(offset),
            "use a range-capable server, or play this uri linearly"));
  }
  return static_cast<int>(ctx.received);
}

void CurlDataSource::CloseLocked() {
  if (easy_ != nullptr) {
    curl_easy_cleanup(static_cast<CURL*>(easy_));
    easy_ = nullptr;
  }
  if (request_headers_ != nullptr) {
    curl_slist_free_all(static_cast<curl_slist*>(request_headers_));
    request_headers_ = nullptr;
  }
  if (resolve_list_ != nullptr) {
    curl_slist_free_all(static_cast<curl_slist*>(resolve_list_));
    resolve_list_ = nullptr;
  }
  active_request_offset_ = -1;
}

MediaError CurlDataSource::CurlErrorLocked(int curl_code) {
  std::string detail =
      "url = " + uri_ + ", curl = " + std::to_string(curl_code);
  if (error_buffer_[0] != '\0') {
    detail += ", msg = " + std::string(error_buffer_);
  }
  std::string summary = "the curl transfer failed";
  std::string suggestion = "check the network, or raise config.net reconnect";
  switch (curl_code) {
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_RESOLVE_PROXY:
    summary = "the host could not be resolved";
    suggestion = "check dns_overrides, or fix the host name";
    return MediaError::Of(ErrorCode::kNetworkUnreachable, summary, detail,
                          suggestion);
  case CURLE_COULDNT_CONNECT:
    summary = "the connection could not be established";
    suggestion = "check the host and proxy settings";
    return MediaError::Of(ErrorCode::kNetworkUnreachable, summary, detail,
                          suggestion);
  case CURLE_OPERATION_TIMEDOUT:
    summary = "the transfer timed out";
    suggestion = "raise config.net timeouts, or check the link quality";
    return MediaError::Of(ErrorCode::kNetworkTimeout, summary, detail,
                          suggestion);
  default:
    break;
  }
  return MediaError::Of(ErrorCode::kSourceReadFailed, summary, detail,
                        suggestion);
}

MediaError CurlDataSource::HttpErrorLocked(int status,
                                           const std::string& curl_detail) {
  std::string detail = "url = " + uri_ + ", http = " + std::to_string(status);
  if (!curl_detail.empty()) {
    detail += ", " + curl_detail;
  }
  if (status == 404) {
    return MediaError::Of(ErrorCode::kSourceNotFound, "the url was not found",
                          detail,
                          "verify the url, or check that the resource still "
                          "exists");
  }
  if (status == 403) {
    return MediaError::Of(ErrorCode::kSourcePermissionDenied,
                          "the url refused access", detail,
                          "check referer/auth headers in the data source "
                          "config");
  }
  return MediaError::Of(ErrorCode::kSourceReadFailed,
                        "the url returned an error status", detail,
                        "inspect the status code and server logs");
}

}  // namespace xmedia::media::curl
