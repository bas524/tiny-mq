//
// Created by Alexander Bychuk on 05.03.2022.
//

#include <vector>
#include <Poco/Base64Encoder.h>
#include "BytesMessage.h"

namespace tiny_mq {
BytesMessage::BytesMessage(const BytesVector &bytes) { data = bytes; }
BytesMessage::BytesMessage(const int8_t *bytes, size_t size) { data = BytesVector(bytes, bytes + size); }
const BytesVector &BytesMessage::bytes() const { return Poco::RefAnyCast<BytesVector>(data); }
void BytesMessage::setBytes(const BytesVector &bytes) { data = bytes; }
void BytesMessage::setBytes(const int8_t *bytes, size_t size) { data = BytesVector(bytes, bytes + size); }
void BytesMessage::writeBytes(const int8_t *bytes, size_t offset, size_t size) {
  const size_t dataSize = offset + size;
  if (data.empty()) {
    data = BytesVector(dataSize, 0);
  }
  auto &vec = Poco::RefAnyCast<BytesVector>(data);
  if (vec.size() < dataSize) {
    vec.reserve(dataSize);
    std::fill(vec.begin() + offset, vec.end(), 0);
  }
  for (size_t i = offset; i < dataSize; ++i) {
    vec[i] = bytes[i - offset];
  }
}
nonstd::span<const int8_t> BytesMessage::readBytes(size_t offset, size_t size) const { return {bytes().begin() + offset, size}; }
void BytesMessage::clearData() { data = BytesVector(); }
size_t BytesMessage::dataSize() const { return bytes().size(); }
Poco::JSON::Object BytesMessage::toJSON() const {
  Poco::JSON::Object json = propertiesToJSON();
  std::ostringstream ostr;
  Poco::Base64Encoder encoder(ostr);
  for (auto c : bytes()) {
    encoder << c;
  }
  encoder.close();
  json.set("data", ostr.str());
  return json;
}
Message::Ptr BytesMessage::copy() const { return tiny_mq::Message::Ptr(new BytesMessage(*this)); }
}  // namespace tiny_mq