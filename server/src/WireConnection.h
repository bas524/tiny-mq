#ifndef TINY_MQ_WIRE_CONNECTION_H
#define TINY_MQ_WIRE_CONNECTION_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace tiny_mq {

using WireTime = std::chrono::steady_clock::time_point;

struct WireLimits {
  std::size_t frame_bytes = 1048576;
  std::size_t header_bytes = 8192;
  std::size_t header_count = 64;
  std::chrono::milliseconds handshake{5000};
  std::size_t output_bytes = 1056768;
  std::chrono::milliseconds flush{1000};
  std::chrono::milliseconds heartbeat_send{10000};
  std::chrono::milliseconds heartbeat_receive{10000};
};

struct WireBytes {
  std::span<const std::byte> bytes;
};
struct WireTimer {};
struct WireEof {};
enum class WireFailureKind { read, write, internal };
struct WireFailure {
  WireFailureKind kind;
};
struct WireStop {};
struct WireWritten {
  std::size_t bytes;
};
using WireEvent = std::variant<WireBytes, WireTimer, WireEof, WireFailure, WireStop, WireWritten>;

struct WireInput {
  WireTime now;
  std::size_t output_budget;
  WireEvent event;
};

enum class WireClose { keep, immediate, after_flush };
struct WireResult {
  std::vector<std::vector<std::byte>> output;
  std::optional<WireTime> deadline;
  WireClose close = WireClose::keep;
};

class WireConnection {
 public:
  virtual ~WireConnection() noexcept = default;
  virtual WireResult handle(const WireInput& input) = 0;
};

}  // namespace tiny_mq

#endif
