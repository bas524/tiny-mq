#include "QueueBrowser.h"

namespace tiny_mq {
QueueBrowser::QueueBrowser(Destination::Ptr destination, std::shared_ptr<Selector> selector)
    : _destination(std::move(destination)), _selector(std::move(selector)) {}

std::vector<Message::Ptr> QueueBrowser::enumerate() const {
  return _destination->_queue->snapshot(_selector.get());
}
}
