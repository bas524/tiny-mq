//
// Created by Alexander Bychuk on 05.03.2022.
//

#ifndef TINY_MQ__BYTESMESSAGE_H_
#define TINY_MQ__BYTESMESSAGE_H_

#include "Message.h"
#include "nonstd/span.hpp"

namespace tiny_mq {
class BytesMessage : public Message {
 public:
  using Ptr = std::shared_ptr<BytesMessage>;
  BytesMessage() = default;
  explicit BytesMessage(const BytesVector &bytes);
  explicit BytesMessage(const int8_t *bytes, size_t size);
  ~BytesMessage() override = default;
  const BytesVector &bytes() const;
  void setBytes(const BytesVector &bytes);
  void setBytes(const int8_t *bytes, size_t size);
  void writeBytes(const int8_t *bytes, size_t offset, size_t size);
  nonstd::span<const int8_t> readBytes(size_t offset, size_t size) const;
  void clearData() override;
  size_t dataSize() const;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__BYTESMESSAGE_H_
