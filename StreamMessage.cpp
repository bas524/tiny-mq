//
// Created by Alexander Bychuk on 06.04.2022.
//

#include "StreamMessage.h"
namespace tiny_mq {
property::ValueType StreamMessage::nextValueType() const { return _stream.nextValueType(); }
void StreamMessage::clearData() { _stream.clear(); }
Poco::JSON::Object StreamMessage::toJSON() const {
  auto json = propertiesToJSON();
  json.set("data", _stream.toJSON());
  return json;
}
Message::Ptr StreamMessage::copy() const { return tiny_mq::Message::Ptr(new StreamMessage(*this)); }
bool StreamMessage::empty() const { return _stream.empty(); }
void StreamMessage::reset() { _stream.reset(); }
bool StreamMessage::eof() const { return _stream.eof(); }
}  // namespace tiny_mq
