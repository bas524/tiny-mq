//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__PRODUCER_H_
#define TINY_MQ__PRODUCER_H_

#include <memory>
#include <Poco/Logger.h>
#include "Message.h"
#include "BlockingConcurrentQueueHeader.h"

namespace tiny_mq {
class Destination;

class Producer {
  Poco::UUID _uuid;
  Destination &_destination;
  std::unique_ptr<moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t> _token;
  Poco::Logger &_logger;

 public:
  using Ptr = std::shared_ptr<Producer>;
  void send(const Message &message);
  virtual ~Producer();

 private:
  explicit Producer(Destination &destination,
                    const Poco::UUID &uuid,
                    std::unique_ptr<moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t> token);
  const moodycamel::BlockingConcurrentQueue<Message::Ptr>::producer_token_t &token() const;
  friend class Destination;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__PRODUCER_H_
