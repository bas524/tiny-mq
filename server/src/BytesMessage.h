//
// Created by Alexander Bychuk on 05.03.2022.
//

#ifndef TINY_MQ__BYTESMESSAGE_H_
#define TINY_MQ__BYTESMESSAGE_H_

#include "Message.h"
#include "nonstd/span.hpp"

namespace tiny_mq {
class BytesMessage : public Message {
  BytesVector _data;

 public:
  using Ptr = std::shared_ptr<BytesMessage>;
  BytesMessage() = default;
  BytesMessage(Poco::UUID uuid_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  explicit BytesMessage(Poco::UUID uuid_, BytesVector bytes_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  explicit BytesMessage(Poco::UUID uuid_, const int8_t *bytes_, size_t size_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  ~BytesMessage() override = default;

  BytesMessage(const BytesMessage &) = default;
  BytesMessage(BytesMessage &&) = default;
  BytesMessage &operator=(const BytesMessage &) = default;
  BytesMessage &operator=(BytesMessage &&) = default;

  const BytesVector &bytes() const;
  void setBytes(const BytesVector &bytes);
  void setBytes(const int8_t *bytes, size_t size);
  void writeBytes(const int8_t *bytes, size_t offset, size_t size);
  nonstd::span<const int8_t> readBytes(size_t offset, size_t size) const;
  void clearData() override;
  size_t dataSize() const;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
  Message::Type type() const override;

  BytesVector dataAsBytes() const override;
  void setDataFromBytes(const BytesVector &bytes) override;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__BYTESMESSAGE_H_
