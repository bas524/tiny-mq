#ifndef TINY_MQ__QUEUE_BROWSER_H_
#define TINY_MQ__QUEUE_BROWSER_H_

#include "Destination.h"

namespace tiny_mq {
class QueueBrowser {
 public:
  std::vector<Message::Ptr> enumerate() const;

 private:
  QueueBrowser(Destination::Ptr destination, std::shared_ptr<Selector> selector);
  Destination::Ptr _destination;
  std::shared_ptr<Selector> _selector;

  friend class Session;
};
}
#endif
