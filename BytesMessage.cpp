//
// Created by Alexander Bychuk on 05.03.2022.
//

#include <vector>
#include <Poco/Base64Encoder.h>
#include "BytesMessage.h"

namespace tiny_mq {
BytesMessage::BytesMessage(const std::vector<uint8_t> &bytes) { data = bytes; }
BytesMessage::BytesMessage(const uint8_t *bytes, size_t size) { data = std::vector<uint8_t>(bytes, bytes + size); }
const std::vector<uint8_t> &BytesMessage::bytes() const { return Poco::RefAnyCast<std::vector<uint8_t>>(data); }
void BytesMessage::setBytes(const std::vector<uint8_t> &bytes) { data = bytes; }
void BytesMessage::setBytes(const uint8_t *bytes, size_t size) { data = std::vector<uint8_t>(bytes, bytes + size); }
void BytesMessage::writeBytes(const uint8_t *bytes, size_t offset, size_t size) {
  const size_t dataSize = offset + size;
  if (data.empty()) {
    data = std::vector<uint8_t>(dataSize, 0);
  }
  auto &vec = Poco::RefAnyCast<std::vector<uint8_t>>(data);
  if (vec.size() < dataSize) {
    vec.reserve(dataSize);
    std::fill(vec.begin() + offset, vec.end(), 0);
  }
  for (size_t i = offset; i < dataSize; ++i) {
    vec[i] = bytes[i - offset];
  }
}
nonstd::span<const uint8_t> BytesMessage::readBytes(size_t offset, size_t size) const { return {bytes().begin() + offset, size}; }
void BytesMessage::clearData() { data = std::vector<uint8_t>(); }
size_t BytesMessage::dataSize() const { return bytes().size(); }
Poco::JSON::Object BytesMessage::toJSON() const {
  Poco::JSON::Object json = propertytoJSON();
  std::ostringstream ostr;
  Poco::Base64Encoder encoder(ostr);
  for (uint8_t c : bytes()) {
    encoder << c;
  }
  encoder.close();
  json.set("data", ostr.str());
  return json;
}
Message::Ptr BytesMessage::copy() const { return tiny_mq::Message::Ptr(new BytesMessage(*this)); }
}  // namespace tiny_mq