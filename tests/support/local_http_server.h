// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_TESTS_SUPPORT_LOCAL_HTTP_SERVER_H_
#define AVBASE_TESTS_SUPPORT_LOCAL_HTTP_SERVER_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace avbase::media::test {

// A minimal HTTP/1.1 server for data-source unit tests, bound to a loopback
// port chosen by the OS. It exists because the network semantics under test
// (Range answers, keep-alive connection reuse, custom headers arriving, a
// stalled body to abort against) need a real socket peer, and the repository
// deliberately keeps its unit tests offline-capable: the python httpd in
// tools/check_protocols.py is an end-to-end gate, not something a unit test
// can spawn.
//
// BEHAVIOUR. One thread serves one accept loop; each connection is handled
// with HTTP/1.1 keep-alive (requests are served back-to-back until the
// client closes or sends "Connection: close"), which is what lets a test
// observe connection reuse through connection_count(). Every served request
// updates last_request_line()/last_headers() so a test can assert that the
// client actually sent its custom headers.
//
// Not production anything: no TLS, no chunked encoding, one request body
// ignored (GET only).
class LocalHttpServer final {
 public:
  struct File {
    std::string body;
    std::string content_type = "application/octet-stream";
    // Answer 200 (full body) even when the request carries a Range header.
    bool ignore_range = false;
    // /drip paths only: sleep this long between 256-byte body chunks, so a
    // reader stays blocked long enough for an Abort() to be observed.
    int drip_ms_per_chunk = 0;
  };

  LocalHttpServer() = default;
  LocalHttpServer(const LocalHttpServer&) = delete;
  LocalHttpServer& operator=(const LocalHttpServer&) = delete;
  ~LocalHttpServer();

  // Binds 127.0.0.1 on an ephemeral port and starts the serve thread.
  // Returns false (and leaves port() at 0) when the socket cannot be created
  // so the test can GTEST_SKIP() instead of hanging.
  bool Start();
  int port() const { return port_; }
  std::string UrlFor(const std::string& path) const;

  void SetFile(const std::string& path, File file);

  // Observability for the assertions above.
  int connection_count() const;
  int request_count() const;
  std::string last_request_line() const;
  // Header names lower-cased, values verbatim, most recent request wins.
  std::map<std::string, std::string> last_headers() const;

 private:
  void ServeLoop();
  void HandleConnection(int fd);

  std::atomic<int> port_{0};
  std::atomic<bool> stopping_{false};
  int listen_fd_ = -1;
  std::thread serve_thread_;

  mutable std::mutex lock_;
  std::map<std::string, File> files_;
  int connection_count_ = 0;
  int request_count_ = 0;
  std::string last_request_line_;
  std::map<std::string, std::string> last_headers_;
};

}  // namespace avbase::media::test

#endif  // AVBASE_TESTS_SUPPORT_LOCAL_HTTP_SERVER_H_
