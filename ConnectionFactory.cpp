//
// ConnectionFactory implementation.
//

#include "ConnectionFactory.h"
#include "Exchange.h"

namespace tiny_mq {

ConnectionFactory::ConnectionFactory(Exchange &exchange) : _exchange(exchange) {}

std::unique_ptr<Connection> ConnectionFactory::createConnection() {
  return std::make_unique<Connection>(_exchange.get());
}

}  // namespace tiny_mq
