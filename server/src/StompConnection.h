#ifndef TINY_MQ_STOMP_CONNECTION_H
#define TINY_MQ_STOMP_CONNECTION_H

#include "WireConnection.h"

#include <memory>

namespace tiny_mq {

class StompConnection final : public WireConnection {
 public:
  StompConnection(WireLimits limits, WireTime accepted);
  ~StompConnection() noexcept override;
  WireResult handle(const WireInput& input) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tiny_mq

#endif
