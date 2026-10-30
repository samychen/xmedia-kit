// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `base/logging.h` (BSD-3-Clause).

#ifndef XMEDIA_BASE_LOGGING_H_
#define XMEDIA_BASE_LOGGING_H_

#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#include "base/base_export.h"

namespace xmedia {
namespace base {
namespace logging {

// LOG_VERBOSE..LOG_FATAL are macro-level names: LOG(INFO) expands to
// LOG_INFO, so they are UPPER_CASE like macros instead of k-prefixed like
// every other enum constant in the tree -- and renaming them would rename
// the public logging API.
// Suppressed as a block, and in the bare form: cpplint does not know
// clang-tidy's check names and reports every one of them as "Unknown NOLINT
// error category". The block is 5 lines, all of them the deliberate
// UPPER_CASE spellings described above.
// NOLINTBEGIN
enum LogSeverity : int {
  LOG_VERBOSE = -1,  // Used by VLOG(n); n maps to -n.
  LOG_INFO = 0,
  LOG_WARNING = 1,
  LOG_ERROR = 2,
  LOG_FATAL = 3,  // Logs, then aborts.
};
// NOLINTEND

// Embedders install a delegate to route xmedia-kit logs into their own logging
// system (Android logcat, spdlog, journald, ...). See docs/10 §11.1.
class XMEDIA_BASE_EXPORT LoggingDelegate {
 public:
  virtual ~LoggingDelegate() = default;
  virtual void OnLogMessage(LogSeverity severity, const char* file, int line,
                            std::string_view message) = 0;
};

// Takes ownership. Pass nullptr to restore the default stderr logger.
// Thread-safe. Must not be called from inside a log statement.
XMEDIA_BASE_EXPORT void SetDelegate(std::unique_ptr<LoggingDelegate> delegate);

XMEDIA_BASE_EXPORT void SetMinLogLevel(LogSeverity severity);
XMEDIA_BASE_EXPORT LogSeverity GetMinLogLevel();
XMEDIA_BASE_EXPORT bool ShouldCreateLogMessage(LogSeverity severity);

// VLOG(n) is enabled when n <= GetMinVLogLevel(). Default: 0 (VLOG off).
XMEDIA_BASE_EXPORT void SetMinVLogLevel(int level);
XMEDIA_BASE_EXPORT int GetMinVLogLevel();
XMEDIA_BASE_EXPORT bool ShouldCreateVerboseMessage(int verbosity);

class XMEDIA_BASE_EXPORT LogMessage {
 public:
  LogMessage(const char* file, int line, LogSeverity severity);
  LogMessage(const LogMessage&) = delete;
  LogMessage& operator=(const LogMessage&) = delete;
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  const char* file_;
  int line_;
  LogSeverity severity_;
  std::ostringstream stream_;
};

// Helper that makes `cond ? (void)0 : stream << ...` type-correct: the two
// ternary arms must share a type, so the logging arm is folded through
// operator& which returns void.
//
// Templated rather than taking std::ostream& because media/base/media_log.h
// reuses the same idiom with MediaLogRecord, which streams into an internal
// buffer and is not an ostream.
class LogMessageVoidify {
 public:
  template <typename T>
  void operator&(T&&) {}
};

}  // namespace logging
}  // namespace base
}  // namespace xmedia

#define XMEDIA_LOG_STREAM(severity) \
  ::xmedia::base::logging::LogMessage(__FILE__, __LINE__, severity).stream()

#define XMEDIA_LOG_IS_ON(severity)                 \
  ::xmedia::base::logging::ShouldCreateLogMessage( \
      ::xmedia::base::logging::LOG_##severity)

// Deliberately NOT parenthesised as a whole, even though the replacement list
// is one ternary. Callers write the `<<` OUTSIDE the macro (`LOG(INFO) << x`),
// and `&` binds looser than `<<`, so the open form parses as
// `voidify & (stream << x)`. Parenthesised, the `<<` instead applies to the
// whole ternary -- whose type is void, because one arm is `(void)0` -- which
// does not compile. bugprone-macro-parentheses cannot see the caller, and its
// diagnostic lands on the macro's LAST line, so the whole definition is
// suppressed as a block: a next-line suppression would have to sit inside
// the backslash continuation to reach that line.
// NOLINTBEGIN
#define XMEDIA_LAZY_STREAM(stream, condition) \
  !(condition) ? (void)0                      \
               : ::xmedia::base::logging::LogMessageVoidify() & (stream)
// NOLINTEND

#define LOG(severity)                                             \
  XMEDIA_LAZY_STREAM(                                             \
      XMEDIA_LOG_STREAM(::xmedia::base::logging::LOG_##severity), \
      XMEDIA_LOG_IS_ON(severity))

#define LOG_IF(severity, condition) !(condition) ? (void)0 : LOG(severity)

#if defined(XMEDIA_ENABLE_DCHECK)
#define DLOG(severity) LOG(severity)
#else
#define DLOG(severity) \
  XMEDIA_LAZY_STREAM(  \
      XMEDIA_LOG_STREAM(::xmedia::base::logging::LOG_##severity), false)
#endif

#define VLOG_IS_ON(verbosity) \
  ::xmedia::base::logging::ShouldCreateVerboseMessage(verbosity)

#define VLOG(verbosity)                                                       \
  XMEDIA_LAZY_STREAM(XMEDIA_LOG_STREAM(::xmedia::base::logging::LOG_VERBOSE), \
                     VLOG_IS_ON(verbosity))

#if defined(XMEDIA_ENABLE_DCHECK)
#define DVLOG(verbosity) VLOG(verbosity)
#else
#define DVLOG(verbosity)                                                      \
  XMEDIA_LAZY_STREAM(XMEDIA_LOG_STREAM(::xmedia::base::logging::LOG_VERBOSE), \
                     false)
#endif

#endif  // XMEDIA_BASE_LOGGING_H_
