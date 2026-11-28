#ifndef TINY_MQ_BROKER_SERVER_H
#define TINY_MQ_BROKER_SERVER_H

#include "WireConnection.h"

#include <Poco/Net/StreamSocket.h>
#include <functional>
#include <memory>

namespace tiny_mq {

using WireFactory = std::function<std::unique_ptr<WireConnection>(const WireLimits&, WireTime)>;
using WireClock = std::function<WireTime()>;
enum class WireWriteStatus { progress, would_block, failed };
struct WireWriteResult {
  WireWriteStatus status;
  std::size_t bytes;
};
using WireWriter = std::function<WireWriteResult(Poco::Net::StreamSocket&, std::span<const std::byte>)>;

struct BrokerDependencies {
  WireFactory factory;
  WireClock clock;
  WireWriter writer;
};

class BrokerServer final {
 public:
  explicit BrokerServer(WireLimits limits = {});
  BrokerServer(WireLimits limits, BrokerDependencies dependencies);
  static BrokerDependencies productionDependencies();
  ~BrokerServer() noexcept;
  std::uint16_t startLoopback();
  void notifyClockChanged();
  void stop() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace tiny_mq

#endif
