//
// ConnectionFactory — JMS-style creator of Connections.
// In-process / server side: bound to an Exchange.
//

#ifndef TINY_MQ__CONNECTION_FACTORY_H_
#define TINY_MQ__CONNECTION_FACTORY_H_

#include "Connection.h"
#include <functional>
#include <memory>

namespace tiny_mq {
class Exchange;

class ConnectionFactory {
 public:
  explicit ConnectionFactory(Exchange &exchange);

  std::unique_ptr<Connection> createConnection();

 private:
  std::reference_wrapper<Exchange> _exchange;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__CONNECTION_FACTORY_H_
