#include "StompConnection.h"
#include "StompCodec.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace tiny_mq {
namespace {
using Millis = std::chrono::milliseconds;
bool durationFits(Millis value, unsigned factor = 1) {
  const auto maximum = std::chrono::duration_cast<Millis>(WireTime::duration::max()).count();
  return value.count() >= 0 && value.count() <= maximum / factor;
}
WireTime later(WireTime now, Millis delay, unsigned factor = 1) {
  if (!durationFits(delay, factor)) { throw std::runtime_error("syntax"); }
  const auto duration = std::chrono::duration_cast<WireTime::duration>(delay * factor);
  if (now > WireTime::max() - duration) { throw std::runtime_error("syntax"); }
  return now + duration;
}
Millis decimal(std::string_view value) {
  if (value.empty()) { throw std::runtime_error("syntax"); }
  Millis::rep number = 0;
  for (unsigned char c : value) {
    if (c < '0' || c > '9' || number > (Millis::max().count() - (c - '0')) / 10) {
      throw std::runtime_error("syntax");
    }
    number = number * 10 + (c - '0');
  }
  const Millis result(number);
  if (!durationFits(result)) { throw std::runtime_error("syntax"); }
  return result;
}
const std::string* header(const StompFrame& frame, std::string_view key) {
  for (const auto& [name, value] : frame.headers) {
    if (name == key) { return &value; }
  }
  return nullptr;
}
std::set<std::string> capabilities(const std::string* value) {
  std::set<std::string> result;
  if (!value) { return result; }
  std::size_t begin = 0;
  do {
    auto end = value->find(',', begin);
    if (end == std::string::npos) { end = value->size(); }
    auto token = value->substr(begin, end - begin);
    auto initial = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (token.empty() || !initial(token.front()) ||
        !std::all_of(token.begin(), token.end(), [&](char c) { return initial(c) || c == '-'; }) ||
        !result.insert(token).second) { throw std::runtime_error("capability"); }
    begin = end + 1;
  } while (begin <= value->size());
  return result;
}
void version(const std::string* value) {
  if (!value) { throw std::runtime_error("version"); }
  bool selected = false;
  std::size_t begin = 0;
  do {
    auto end = value->find(',', begin);
    if (end == std::string::npos) { end = value->size(); }
    const std::string_view token(value->data() + begin, end - begin);
    const auto dot = token.find('.');
    auto digits = [](std::string_view text) {
      return !text.empty() && std::all_of(text.begin(), text.end(),
          [](char c) { return c >= '0' && c <= '9'; });
    };
    if (dot == std::string_view::npos || !digits(token.substr(0, dot)) ||
        !digits(token.substr(dot + 1))) { throw std::runtime_error("syntax"); }
    selected = selected || token == "1.2";
    begin = end + 1;
  } while (begin <= value->size());
  if (!selected) { throw std::runtime_error("version"); }
}
std::size_t receiptSize(const std::string& id, std::size_t budget) {
  std::size_t size = 22;
  if (size > budget) { throw std::length_error("budget"); }
  for (char c : id) {
    const std::size_t count = (c == '\\' || c == '\r' || c == '\n' || c == ':') ? 2 : 1;
    if (count > budget - size) { throw std::length_error("budget"); }
    size += count;
  }
  return size;
}
std::string receiptFrame(const std::string& id, std::size_t size) {
  std::string result;
  result.reserve(size);
  result = "RECEIPT\nreceipt-id:";
  for (char c : id) {
    switch (c) {
      case '\\': result += "\\\\"; break;
      case '\r': result += "\\r"; break;
      case '\n': result += "\\n"; break;
      case ':': result += "\\c"; break;
      default: result += c;
    }
  }
  result += "\n\n";
  result += '\0';
  return result;
}
struct Finished {};
}

struct StompConnection::Impl {
  WireLimits limits;
  StompCodec codec;
  WireTime handshakeDeadline;
  bool connected = false;
  bool confirmed = false;
  bool closed = false;
  std::size_t connectedRemaining = 0;
  std::size_t pending = 0;
  Millis incoming{0};
  Millis outgoing{0};
  WireTime lastRx{};
  WireTime lastTx{};

  Impl(WireLimits settings, WireTime accepted)
      : limits(settings), codec(settings), handshakeDeadline(later(accepted, settings.handshake)) {}

  static WireResult immediate() { return {{}, std::nullopt, WireClose::immediate}; }
  void append(WireResult& result, const std::string& text, std::size_t& budget) {
    if (text.size() > budget || text.size() > std::numeric_limits<std::size_t>::max() - pending) {
      throw std::length_error("budget");
    }
    budget -= text.size();
    pending += text.size();
    const auto data = std::as_bytes(std::span(text.data(), text.size()));
    result.output.emplace_back(data.begin(), data.end());
  }
  std::optional<WireTime> deadline() const {
    if (!confirmed) { return handshakeDeadline; }
    std::optional<WireTime> result;
    if (incoming.count() != 0) { result = later(lastRx, incoming, 2); }
    if (outgoing.count() != 0 && pending == 0) {
      const auto next = later(lastTx, outgoing);
      if (!result || next < *result) { result = next; }
    }
    return result;
  }
  void drain(WireResult& result) {
    result.close = WireClose::after_flush;
    result.deadline = connected && !confirmed ? std::optional{handshakeDeadline} : std::nullopt;
    closed = true;
  }
  void dispatch(StompFrame&& frame, WireResult& result, std::size_t& budget, WireTime now) {
    if (!connected) {
      if (frame.command != "CONNECT" && frame.command != "STOMP") {
        throw std::runtime_error("unsupported-command");
      }
      version(header(frame, "accept-version"));
      const auto* host = header(frame, "host");
      if (!host || host->empty()) { throw std::runtime_error("syntax"); }
      Millis clientSend{0}, clientReceive{0};
      if (const auto* beat = header(frame, "heart-beat")) {
        const auto comma = beat->find(',');
        if (comma == std::string::npos) { throw std::runtime_error("syntax"); }
        clientSend = decimal(std::string_view(*beat).substr(0, comma));
        clientReceive = decimal(std::string_view(*beat).substr(comma + 1));
      }
      incoming = clientSend.count() && limits.heartbeat_receive.count()
          ? std::max(clientSend, limits.heartbeat_receive) : Millis{0};
      outgoing = clientReceive.count() && limits.heartbeat_send.count()
          ? std::max(clientReceive, limits.heartbeat_send) : Millis{0};
      // Validate even the latest possible timely confirmation before issuing CONNECTED.
      static_cast<void>(later(handshakeDeadline, incoming, 2));
      static_cast<void>(later(handshakeDeadline, outgoing));
      const auto offered = capabilities(header(frame, "x-tinymq-capabilities"));
      const auto required = capabilities(header(frame, "x-tinymq-required-capabilities"));
      for (const auto& token : required) {
        if (token != "framing-v1" || !offered.contains(token)) { throw std::runtime_error("capability"); }
      }
      const auto send = std::to_string(limits.heartbeat_send.count());
      const auto receive = std::to_string(limits.heartbeat_receive.count());
      constexpr std::string_view prefix = "CONNECTED\nversion:1.2\nheart-beat:";
      constexpr std::string_view capability = "x-tinymq-capabilities:framing-v1\n";
      const bool framing = offered.contains("framing-v1");
      const auto responseBytes = prefix.size() + send.size() + 1 + receive.size() + 3 +
          (framing ? capability.size() : 0);
      const auto* id = header(frame, "receipt");
      if (responseBytes > budget) { throw std::length_error("budget"); }
      const auto receiptBytes = id ? receiptSize(*id, budget - responseBytes) : 0;
      std::string response;
      response.reserve(responseBytes);
      response += prefix;
      response += send;
      response += ',';
      response += receive;
      response += '\n';
      if (framing) { response += capability; }
      response += '\n'; response += '\0';
      connectedRemaining = response.size();
      append(result, response, budget);
      if (id) { append(result, receiptFrame(*id, receiptBytes), budget); }
      connected = true;
      lastRx = lastTx = now;
    } else if (frame.command == "DISCONNECT") {
      if (const auto* id = header(frame, "receipt")) {
        const auto size = receiptSize(*id, budget);
        append(result, receiptFrame(*id, size), budget);
      }
      drain(result);
      throw Finished{};
    } else { throw std::runtime_error("unsupported-command"); }
  }

  WireResult handle(const WireInput& input) {
    if (closed) { return immediate(); }
    if (std::holds_alternative<WireEof>(input.event) ||
        std::holds_alternative<WireFailure>(input.event) ||
        std::holds_alternative<WireStop>(input.event)) {
      closed = true; codec.eof(); return immediate();
    }
    WireResult result;
    auto budget = input.output_budget;
    try {
      if (!confirmed && input.now >= handshakeDeadline) {
        if (connected) { closed = true; return immediate(); }
        throw std::runtime_error("handshake-timeout");
      }
      if (const auto* bytes = std::get_if<WireBytes>(&input.event)) {
        if (confirmed && !bytes->bytes.empty()) { lastRx = input.now; }
        codec.feed(bytes->bytes, [&](StompFrame&& frame) {
          dispatch(std::move(frame), result, budget, input.now);
        });
      } else if (const auto* written = std::get_if<WireWritten>(&input.event)) {
        if (written->bytes == 0 || written->bytes > pending) { closed = true; return immediate(); }
        pending -= written->bytes;
        lastTx = input.now;
        if (connected && !confirmed) {
          connectedRemaining -= std::min(connectedRemaining, written->bytes);
          if (connectedRemaining == 0) { confirmed = true; lastRx = lastTx = input.now; }
        }
      }
      if (confirmed) {
        if (incoming.count() != 0 && input.now >= later(lastRx, incoming, 2)) {
          throw std::runtime_error("heartbeat-timeout");
        }
        if (outgoing.count() != 0 && pending == 0 && input.now >= later(lastTx, outgoing)) {
          append(result, "\n", budget);
        }
      }
      result.deadline = deadline();
    } catch (const Finished&) {
      return result;
    } catch (const std::length_error&) {
      closed = true; return immediate();
    } catch (const std::runtime_error& failure) {
      const std::string_view code(failure.what());
      std::string response = "ERROR\nmessage:";
      response += code;
      response += '\n';
      if (code == "version") { response += "version:1.2\n"; }
      response += '\n'; response += '\0';
      try { append(result, response, budget); }
      catch (const std::length_error&) { closed = true; return immediate(); }
      drain(result);
    }
    return result;
  }
};

StompConnection::StompConnection(WireLimits limits, WireTime accepted)
    : impl_(std::make_unique<Impl>(limits, accepted)) {}
StompConnection::~StompConnection() noexcept = default;
WireResult StompConnection::handle(const WireInput& input) { return impl_->handle(input); }
}  // namespace tiny_mq
