// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tests/support/local_http_server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace xmedia::media::test {
namespace {

// Reads from |fd| until the blank line that ends the header block. Returns
// false on EOF/error/timeout so the caller closes the connection.
bool ReadHeaders(int fd, std::string* out) {
  out->clear();
  char buffer[1024];
  while (out->find("\r\n\r\n") == std::string::npos &&
         out->find("\n\n") == std::string::npos) {
    ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
    if (got <= 0) {
      return false;
    }
    out->append(buffer, static_cast<size_t>(got));
  }
  return true;
}

// "GET /path HTTP/1.1" -> ("/path", "GET"); empty on garbage.
void ParseRequestLine(const std::string& line, std::string* method,
                      std::string* path) {
  const size_t first = line.find(' ');
  const size_t second =
      line.find(' ', first == std::string::npos ? 0 : first + 1);
  if (first == std::string::npos || second == std::string::npos) {
    return;
  }
  *method = line.substr(0, first);
  *path = line.substr(first + 1, second - first - 1);
}

// "Host: x" -> ("host", "x").
bool ParseHeaderLine(const std::string& line, std::string* name,
                     std::string* value) {
  const size_t colon = line.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  *name = line.substr(0, colon);
  for (char& c : *name) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  value->assign(line.substr(colon + 1));
  while (!value->empty() && (value->front() == ' ' || value->front() == '\t')) {
    value->erase(value->begin());
  }
  return true;
}

void SendAll(int fd, const std::string& bytes) {
  size_t sent = 0;
#ifdef SO_NOSIGPIPE
  const int flags = 0;
#else
  const int flags = MSG_NOSIGNAL;
#endif
  while (sent < bytes.size()) {
    ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, flags);
    if (n <= 0) {
      return;
    }
    sent += static_cast<size_t>(n);
  }
}

void SendStatus(int fd, int status, const std::string& reason,
                const std::map<std::string, std::string>& extra_headers,
                const std::string& body) {
  std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                     "\r\nContent-Length: " + std::to_string(body.size()) +
                     "\r\n";
  for (const auto& [name, value] : extra_headers) {
    head += name + ": " + value + "\r\n";
  }
  head += "\r\n";
  SendAll(fd, head);
  SendAll(fd, body);
}

}  // namespace

LocalHttpServer::~LocalHttpServer() {
  stopping_.store(true);
  if (listen_fd_ >= 0) {
    ::shutdown(listen_fd_, SHUT_RDWR);
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  if (serve_thread_.joinable()) {
    serve_thread_.join();
  }
}

bool LocalHttpServer::Start() {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return false;
  }
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
          0 ||
      ::listen(listen_fd_, 4) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) !=
      0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  port_.store(ntohs(addr.sin_port));
  serve_thread_ = std::thread([this] { ServeLoop(); });
  return true;
}

std::string LocalHttpServer::UrlFor(const std::string& path) const {
  return "http://127.0.0.1:" + std::to_string(port_) + path;
}

void LocalHttpServer::SetFile(const std::string& path, File file) {
  std::lock_guard<std::mutex> scoped(lock_);
  files_[path] = std::move(file);
}

int LocalHttpServer::connection_count() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return connection_count_;
}

int LocalHttpServer::request_count() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return request_count_;
}

std::string LocalHttpServer::last_request_line() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return last_request_line_;
}

std::map<std::string, std::string> LocalHttpServer::last_headers() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return last_headers_;
}

void LocalHttpServer::ServeLoop() {
  while (!stopping_.load()) {
    int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      if (stopping_.load()) {
        break;
      }
      continue;
    }
    {
      std::lock_guard<std::mutex> scoped(lock_);
      ++connection_count_;
    }
    HandleConnection(fd);
    ::close(fd);
  }
}

void LocalHttpServer::HandleConnection(int fd) {
  std::string raw;
  while (!stopping_.load()) {
    if (!ReadHeaders(fd, &raw)) {
      return;
    }
    const size_t line_end = raw.find("\r\n");
    const std::string request_line =
        line_end == std::string::npos ? raw : raw.substr(0, line_end);
    std::string method;
    std::string path_with_query;
    ParseRequestLine(request_line, &method, &path_with_query);
    const size_t query = path_with_query.find('?');
    const std::string path = query == std::string::npos
                                 ? path_with_query
                                 : path_with_query.substr(0, query);

    std::map<std::string, std::string> headers;
    size_t cursor = line_end == std::string::npos ? raw.size() : line_end + 2;
    while (cursor < raw.size()) {
      const size_t end = raw.find("\r\n", cursor);
      const std::string header_line = end == std::string::npos
                                          ? raw.substr(cursor)
                                          : raw.substr(cursor, end - cursor);
      std::string name;
      std::string value;
      if (ParseHeaderLine(header_line, &name, &value)) {
        headers[name] = value;
      }
      if (end == std::string::npos) {
        break;
      }
      cursor = end + 2;
    }

    {
      std::lock_guard<std::mutex> scoped(lock_);
      ++request_count_;
      last_request_line_ = request_line;
      last_headers_ = headers;
    }

    File file;
    bool known = false;
    {
      std::lock_guard<std::mutex> scoped(lock_);
      auto it = files_.find(path);
      if (it != files_.end()) {
        file = it->second;
        known = true;
      }
    }
    if (!known) {
      SendStatus(fd, 404, "Not Found", {}, "missing\n");
      if (headers["connection"] == "close") {
        return;
      }
      continue;
    }

    // Range: "bytes=A-B" or "bytes=A-". Both ends matter: a bounded answer
    // is what keeps a client's keep-alive connection alive (an over-long
    // 206 forces the client to abort the excess mid-body, which kills the
    // connection).
    int64_t range_start = -1;
    int64_t range_end = -1;
    auto range = headers.find("range");
    if (range != headers.end() && range->second.starts_with("bytes=")) {
      const std::string spec = range->second.substr(6);
      const size_t dash = spec.find('-');
      if (dash != std::string::npos && dash > 0) {
        range_start = std::strtoll(spec.substr(0, dash).c_str(), nullptr, 10);
        if (dash + 1 < spec.size()) {
          range_end = std::strtoll(spec.substr(dash + 1).c_str(), nullptr, 10);
        }
      }
    }
    const bool want_range = range_start >= 0 && !file.ignore_range;

    if (want_range && static_cast<size_t>(range_start) >= file.body.size()) {
      // Past the end: 416 with the total, empty body -- what a reader treats
      // as EOF.
      SendStatus(
          fd, 416, "Range Not Satisfiable",
          {{"Content-Range", "bytes */" + std::to_string(file.body.size())}},
          "");
      if (headers["connection"] == "close") {
        return;
      }
      continue;
    }

    std::map<std::string, std::string> extra;
    // A server that ignores Range does not advertise byte ranges either --
    // clients legitimately read Accept-Ranges as the seekability signal.
    if (!file.ignore_range) {
      extra["Accept-Ranges"] = "bytes";
    }
    extra["Content-Type"] = file.content_type;
    std::string body;
    int status = 200;
    std::string reason = "OK";
    if (want_range) {
      const size_t start = static_cast<size_t>(range_start);
      size_t end = file.body.size() - 1;
      if (range_end >= 0) {
        end = std::min(end, static_cast<size_t>(range_end));
      }
      body = file.body.substr(start, end - start + 1);
      extra["Content-Range"] = "bytes " + std::to_string(range_start) + "-" +
                               std::to_string(static_cast<int64_t>(end)) + "/" +
                               std::to_string(file.body.size());
      status = 206;
      reason = "Partial Content";
    }
    if (file.drip_ms_per_chunk <= 0) {
      SendStatus(fd, status, reason, extra, body);
    } else {
      // Paced body: the head goes out first, then the body drips in 256-byte
      // chunks with a sleep between them, so a reader stays blocked long
      // enough for an Abort() to be observed mid-read.
      std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason +
                         "\r\nContent-Length: " + std::to_string(body.size()) +
                         "\r\n";
      for (const auto& [name, value] : extra) {
        head += name + ": " + value + "\r\n";
      }
      head += "\r\n";
      SendAll(fd, head);
      for (size_t offset = 0; offset < body.size(); offset += 256) {
        if (stopping_.load()) {
          return;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(file.drip_ms_per_chunk));
        SendAll(fd, body.substr(offset, 256));
      }
    }
    if (headers["connection"] == "close") {
      return;
    }
  }
}

}  // namespace xmedia::media::test
