// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/mpd_parser.h"

#include <cstdlib>
#include <functional>
#include <map>

#include "media/ffmpeg/hls_m3u8_parser.h"  // ResolveUrl, same URI shapes

namespace avbase::media::dash {
namespace {

// The MPD subset tokenizer: a flat element walk with start/end callbacks.
// No namespace resolution (attribute and element names are matched with
// their prefix stripped), no DTD, no entity decoding. Comments and the
// XML declaration are skipped.
struct Element {
  std::string name;                          // prefix stripped
  std::map<std::string, std::string> attrs;  // prefix-stripped, lower names
};

void StripPrefix(std::string* name) {
  const size_t colon = name->find(':');
  if (colon != std::string::npos) {
    name->erase(0, colon + 1);
  }
}

void Lower(std::string* s) {
  for (char& c : *s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
}

// Parses attributes "a=1 b=\"two x=y\"" honouring quotes.
std::map<std::string, std::string> ParseAttrs(std::string_view text) {
  std::map<std::string, std::string> attrs;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' ||
                               text[i] == '\n' || text[i] == '\r')) {
      ++i;
    }
    const size_t eq = text.find('=', i);
    if (eq == std::string_view::npos) {
      break;
    }
    std::string name(text.substr(i, eq - i));
    StripPrefix(&name);
    Lower(&name);
    size_t v = eq + 1;
    std::string value;
    if (v < text.size() && (text[v] == '"' || text[v] == '\'')) {
      const char quote = text[v];
      const size_t close = text.find(quote, v + 1);
      if (close == std::string_view::npos) {
        break;
      }
      value = std::string(text.substr(v + 1, close - v - 1));
      i = close + 1;
    } else {
      size_t end = v;
      while (end < text.size() && text[end] != ' ' && text[end] != '\t' &&
             text[end] != '\n' && text[end] != '\r') {
        ++end;
      }
      value = std::string(text.substr(v, end - v));
      i = end;
    }
    attrs[name] = value;
  }
  return attrs;
}

bool WalkElements(
    std::string_view text,
    const std::function<void(const Element&, bool self_closing)>& on_start,
    const std::function<void(const std::string& name)>& on_end) {
  size_t i = 0;
  std::vector<std::string> stack;
  while (i < text.size()) {
    const size_t lt = text.find('<', i);
    if (lt == std::string_view::npos) {
      break;
    }
    if (text.substr(lt).starts_with("<!--")) {
      const size_t end = text.find("-->", lt);
      if (end == std::string_view::npos) {
        return false;
      }
      i = end + 3;
      continue;
    }
    if (text.substr(lt).starts_with("<?") ||
        text.substr(lt).starts_with("<!")) {
      const size_t end = text.find('>', lt);
      if (end == std::string_view::npos) {
        return false;
      }
      i = end + 1;
      continue;
    }
    if (text.substr(lt).starts_with("</")) {
      const size_t end = text.find('>', lt);
      if (end == std::string_view::npos) {
        return false;
      }
      std::string name(text.substr(lt + 2, end - lt - 2));
      StripPrefix(&name);
      on_end(name);
      if (!stack.empty()) {
        stack.pop_back();
      }
      i = end + 1;
      continue;
    }
    const size_t end = text.find('>', lt);
    if (end == std::string_view::npos) {
      return false;
    }
    std::string_view inner = text.substr(lt + 1, end - lt - 1);
    const bool self_closing = inner.ends_with('/');
    if (self_closing) {
      inner.remove_suffix(1);
    }
    size_t name_end = 0;
    while (name_end < inner.size() && inner[name_end] != ' ' &&
           inner[name_end] != '\t' && inner[name_end] != '\n') {
      ++name_end;
    }
    Element element;
    element.name = std::string(inner.substr(0, name_end));
    StripPrefix(&element.name);
    element.attrs =
        ParseAttrs(inner.substr(std::min(name_end + 1, inner.size())));
    on_start(element, self_closing);
    if (!self_closing) {
      stack.push_back(element.name);
    }
    i = end + 1;
  }
  return stack.empty();
}

// "PT6H0M30.500S" / "PT96.5S" -> TimeDelta. Only the time part carries
// media time; a date-only duration answers zero.
base::expected<base::TimeDelta, MediaError>
ParseIsoDuration(std::string_view value) {
  if (value.empty() || value[0] != 'P') {
    return base::unexpected(
        MediaError::Of(ErrorCode::kSourceUnsupported, "bad iso duration",
                       "value = " + std::string(value),
                       "expected an ISO-8601 duration like PT30S or PT1M30S"));
  }
  const size_t t = value.find('T');
  if (t == std::string_view::npos) {
    return base::Seconds(0);
  }
  double seconds = 0;
  double current = 0;
  bool have_number = false;
  size_t i = t + 1;
  while (i <= value.size()) {
    const char c = i < value.size() ? value[i] : '\0';
    if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
      have_number = true;
      ++i;
      continue;
    }
    if (c == '.') {
      ++i;
      double frac = 0;
      double scale = 0.1;
      while (i < value.size() && value[i] >= '0' && value[i] <= '9') {
        frac += (value[i] - '0') * scale;
        scale *= 0.1;
        ++i;
      }
      current += frac;
      have_number = true;
      continue;
    }
    if (!have_number) {
      break;
    }
    switch (c) {
    case 'H':
      seconds += current * 3600;
      break;
    case 'M':
      seconds += current * 60;
      break;
    case 'S':
      seconds += current;
      break;
    default:
      break;  // date-part units and anything unknown: ignored
    }
    current = 0;
    have_number = false;
    ++i;
  }
  return base::Milliseconds(static_cast<int64_t>(seconds * 1000.0 + 0.5));
}

int64_t ParseIntOr(const std::string& value, int64_t fallback) {
  if (value.empty()) {
    return fallback;
  }
  int64_t parsed = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return fallback;
    }
    parsed = parsed * 10 + (c - '0');
  }
  return parsed;
}

std::string ExpandTemplate(const std::string& media_template,
                           const std::string& representation_id,
                           int64_t number) {
  std::string out = media_template;
  size_t pos = out.find("$RepresentationID$");
  if (pos != std::string::npos) {
    out.replace(pos, 18, representation_id);
  }
  pos = out.find("$Number$");
  if (pos != std::string::npos) {
    out.replace(pos, 8, std::to_string(number));
  }
  return out;
}

}  // namespace

std::string ResolveMpdUrl(const std::string& base_url,
                          std::string_view relative) {
  return hls::ResolveUrl(base_url, relative);
}

base::expected<Mpd, MediaError> ParseMpd(std::string_view text,
                                         const std::string& base_url) {
  Mpd mpd;

  // SegmentTemplate scope: MPD-level (inherited by every representation)
  // or AdaptationSet-level (overriding). The Representation handler reads
  // whatever was last seen when it opens.
  std::string template_media;
  std::string template_init;
  int64_t template_duration_units = 0;
  int64_t template_start_number = 1;
  int64_t template_timescale = 1;
  std::string current_adaptation_mime;

  auto on_start = [&](const Element& element, bool self_closing) {
    Element e = element;  // mutable copy: attrs lookups insert
    (void)self_closing;
    if (e.name == "MPD") {
      mpd.type = e.attrs["type"];
      mpd.is_live = mpd.type == "dynamic";
      auto duration = ParseIsoDuration(e.attrs["mediapresentationduration"]);
      if (duration.has_value()) {
        mpd.media_presentation_duration = *duration;
      }
      return;
    }
    if (e.name == "SegmentTemplate") {
      template_media = e.attrs["media"];
      template_init = e.attrs["initialization"];
      // duration is in TIMESAULE units -- a plain integer, not ISO.
      template_duration_units = ParseIntOr(e.attrs["duration"], 0);
      template_start_number = ParseIntOr(e.attrs["startnumber"], 1);
      template_timescale = ParseIntOr(e.attrs["timescale"], 1);
      return;
    }
    if (e.name == "AdaptationSet") {
      current_adaptation_mime = e.attrs["mimetype"];
      return;
    }
    if (e.name == "Representation") {
      MpdRepresentation rep;
      rep.id = e.attrs["id"];
      rep.bandwidth = ParseIntOr(e.attrs["bandwidth"], 0);
      rep.mime_type = e.attrs["mimetype"].empty() ? current_adaptation_mime
                                                  : e.attrs["mimetype"];
      rep.codecs = e.attrs["codecs"];
      if (!template_media.empty() && template_duration_units > 0 &&
          template_timescale > 0 &&
          mpd.media_presentation_duration > base::TimeDelta()) {
        const double seg_seconds =
            static_cast<double>(template_duration_units) /
            static_cast<double>(template_timescale);
        rep.segment_duration = base::Milliseconds(
            static_cast<int64_t>(seg_seconds * 1000.0 + 0.5));
        const int64_t count = static_cast<int64_t>(
            mpd.media_presentation_duration / rep.segment_duration + 0.5);
        if (!template_init.empty()) {
          rep.segment_urls.push_back(
              ResolveMpdUrl(base_url, ExpandTemplate(template_init, rep.id,
                                                     template_start_number)));
        }
        for (int64_t n = 0; n < count; ++n) {
          rep.segment_urls.push_back(ResolveMpdUrl(
              base_url, ExpandTemplate(template_media, rep.id,
                                       template_start_number + n)));
          rep.total_duration += rep.segment_duration;
        }
      }
      mpd.representations.push_back(std::move(rep));
    }
  };

  if (!WalkElements(text, on_start, [](const std::string&) {})) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the mpd is not well-formed xml",
        "a start tag never closed, or a comment ran to the end",
        "check that the url serves the manifest, not an error page"));
  }
  if (mpd.representations.empty()) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the mpd names no representation",
        "segments = 0, representations = 0",
        "check that the manifest is an on-demand SegmentTemplate profile"));
  }
  return mpd;
}

}  // namespace avbase::media::dash
