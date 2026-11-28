#ifndef TINY_MQ_STOMP_CODEC_H
#define TINY_MQ_STOMP_CODEC_H

#include "WireConnection.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace tiny_mq {

struct StompFrame {
  std::string command;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::byte> body;
};

class StompCodec final {
 public:
  explicit StompCodec(WireLimits limits);
  ~StompCodec() noexcept;
  void feed(std::span<const std::byte> bytes,
            const std::function<void(StompFrame&&)>& completed);
  void eof() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tiny_mq

#endif
