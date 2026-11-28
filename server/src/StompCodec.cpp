#include "StompCodec.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace tiny_mq {
namespace {
void syntax() { throw std::runtime_error("syntax"); }
void limit() { throw std::runtime_error("limit"); }

void utf8(std::string_view text) {
  std::uint32_t value = 0;
  std::uint32_t minimum = 0;
  unsigned remaining = 0;
  for (unsigned char c : text) {
    if (remaining != 0) {
      if ((c & 0xc0) != 0x80) { syntax(); }
      value = (value << 6) | (c & 0x3f);
      if (--remaining == 0 && (value < minimum || value > 0x10ffff ||
                               (value >= 0xd800 && value <= 0xdfff))) { syntax(); }
    } else if (c < 0x80) {
      continue;
    } else if (c >= 0xc2 && c <= 0xdf) {
      value = c & 0x1f; minimum = 0x80; remaining = 1;
    } else if (c >= 0xe0 && c <= 0xef) {
      value = c & 0x0f; minimum = 0x800; remaining = 2;
    } else if (c >= 0xf0 && c <= 0xf4) {
      value = c & 7; minimum = 0x10000; remaining = 3;
    } else { syntax(); }
  }
  if (remaining != 0) { syntax(); }
}

std::string unescape(std::string_view text, bool literal) {
  utf8(text);
  if (literal) { return std::string(text); }
  std::string result;
  for (std::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (c == '\\') {
      if (++i == text.size()) { syntax(); }
      switch (text[i]) {
        case 'r': c = '\r'; break;
        case 'n': c = '\n'; break;
        case 'c': c = ':'; break;
        case '\\': c = '\\'; break;
        default: syntax();
      }
    }
    result.push_back(c);
  }
  return result;
}
}

struct StompCodec::Impl {
  enum class State { command, headers, body, terminator };
  WireLimits limits;
  State state = State::command;
  StompFrame frame;
  std::string line;
  std::size_t used = 0;
  std::size_t headers = 0;
  std::optional<std::size_t> length;
  bool cr = false;

  explicit Impl(WireLimits settings) : limits(settings) {}

  void reset() noexcept {
    frame = {};
    std::string{}.swap(line);
    state = State::command;
    used = headers = 0;
    length.reset();
    cr = false;
  }

  void endHeaders() {
    state = State::body;
    for (const auto& [name, value] : frame.headers) {
      if (name != "content-length") { continue; }
      if (value.empty() || !std::all_of(value.begin(), value.end(),
          [](unsigned char c) { return c >= '0' && c <= '9'; })) { syntax(); }
      if (used >= limits.frame_bytes) { limit(); }
      const auto budget = limits.frame_bytes - used - 1;
      std::size_t count = 0;
      for (char c : value) {
        const auto digit = static_cast<std::size_t>(c - '0');
        if (digit > budget || count > (budget - digit) / 10) { limit(); }
        count = count * 10 + digit;
      }
      length = count;
      if (count == 0) { state = State::terminator; }
      break;
    }
  }

  void endLine() {
    if (state == State::command) {
      if (line.empty()) { used = 0; return; }
      utf8(line);
      if (line.find('\0') != std::string::npos) { syntax(); }
      frame.command = std::move(line);
      line = {};
      state = State::headers;
    } else {
      if (line.empty()) { endHeaders(); return; }
      if (headers == limits.header_count) { limit(); }
      ++headers;
      const auto colon = line.find(':');
      if (colon == std::string::npos) { syntax(); }
      const bool literal = frame.command == "CONNECT" || frame.command == "CONNECTED";
      auto name = unescape(std::string_view(line).substr(0, colon), literal);
      auto value = unescape(std::string_view(line).substr(colon + 1), literal);
      if (std::none_of(frame.headers.begin(), frame.headers.end(),
          [&](const auto& header) { return header.first == name; })) {
        frame.headers.emplace_back(std::move(name), std::move(value));
      }
      std::string{}.swap(line);
    }
  }

  void complete(const std::function<void(StompFrame&&)>& callback) {
    if (!frame.body.empty() && frame.command != "SEND" && frame.command != "MESSAGE" &&
        frame.command != "ERROR") { syntax(); }
    auto done = std::move(frame);
    reset();
    callback(std::move(done));
  }

  void byte(unsigned char c, const std::function<void(StompFrame&&)>& callback) {
    // Inter-frame EOLs are deliberately outside the frame budget.
    if (state == State::command && line.empty() && (c == '\n' || c == '\r' || cr)) {
      if (cr && c != '\n') { syntax(); }
      cr = c == '\r';
      return;
    }
    if (used == limits.frame_bytes) { limit(); }
    ++used;
    if (state == State::body) {
      if (!length && c == 0) { complete(callback); return; }
      frame.body.push_back(static_cast<std::byte>(c));
      if (length && frame.body.size() == *length) { state = State::terminator; }
      return;
    }
    if (state == State::terminator) {
      if (c != 0) { syntax(); }
      complete(callback);
      return;
    }
    if (cr) {
      if (c != '\n') { syntax(); }
      cr = false;
      endLine();
    } else if (c == '\r') {
      cr = true;
    } else if (c == '\n') {
      endLine();
    } else {
      if (state == State::headers && line.size() == limits.header_bytes) { limit(); }
      if (state == State::command && c == 0) { syntax(); }
      line.push_back(static_cast<char>(c));
    }
  }
};

StompCodec::StompCodec(WireLimits limits) : impl_(std::make_unique<Impl>(limits)) {}
StompCodec::~StompCodec() noexcept = default;
void StompCodec::feed(std::span<const std::byte> bytes,
                      const std::function<void(StompFrame&&)>& completed) {
  for (auto value : bytes) { impl_->byte(std::to_integer<unsigned char>(value), completed); }
}
void StompCodec::eof() noexcept { impl_->reset(); }
}  // namespace tiny_mq
