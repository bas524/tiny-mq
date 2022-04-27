//
// Created by Alexander Bychuk on 06.04.2022.
//

#ifndef TINY_MQ__STREAMMESSAGE_H_
#define TINY_MQ__STREAMMESSAGE_H_

#include "Message.h"

namespace tiny_mq {
class StreamMessage : public Message {
  PropertiesStream _stream;

 public:
  using Ptr = std::shared_ptr<StreamMessage>;
  StreamMessage() = default;
  ~StreamMessage() override = default;
  void clearData() override;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
  template <typename T>
  void write(const T &value) {
    _stream.template write(value);
  }
  template <typename T>
  const T &read() const {
    return _stream.template nextValue<T>();
  }
  property::ValueType nextValueType() const;
  bool empty() const;
  void reset();
  bool eof() const;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__STREAMMESSAGE_H_
