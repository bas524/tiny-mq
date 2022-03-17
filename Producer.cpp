//
// Created by Alexander Bychuk on 10.11.2021.
//

#include "Producer.h"
#include "Destination.h"
#include "LogTracer.h"

namespace tiny_mq {
Producer::Producer(Destination &destination,
                   const Poco::UUID &uuid,
                   std::unique_ptr<moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t> token)
    : _uuid(uuid),
      _destination(destination),
      _token(std::move(token)),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.consumer.%s", _uuid.toString()))) {
  TRACE(_logger);
  poco_information(_logger, Poco::format("create producer[%s] for %s", _uuid.toString(), _destination.uri()));
}
Producer::~Producer() {
  TRACE(_logger);
  poco_information(_logger, Poco::format("destroy producer[%s] for %s", _uuid.toString(), _destination.uri()));
}
void Producer::send(const Message &message) {
  TRACE(_logger);
  _destination.save(*this, message);
  poco_information(_logger, Poco::format("producer[%s] send message[%s] to %s", _uuid.toString(), message.uuid.toString(), _destination.uri()));
}
const moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t &Producer::token() const {
  TRACE(_logger);
  return *_token;
}
}  // namespace tiny_mq
