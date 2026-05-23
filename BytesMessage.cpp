//
// Created by Alexander Bychuk on 05.03.2022.
//

#include <vector>
#include <Poco/Base64Encoder.h>
#include "BytesMessage.h"

namespace tiny_mq {
BytesMessage::BytesMessage(Poco::UUID uuid_, Reliability reliability_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
BytesMessage::BytesMessage(Poco::UUID uuid_, BytesVector bytes_, Reliability reliability_) : _data(std::move(bytes_)) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
BytesMessage::BytesMessage(Poco::UUID uuid_, const int8_t *bytes_, size_t size_, Reliability reliability_) : _data(bytes_, bytes_ + size_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
const BytesVector &BytesMessage::bytes() const { return _data; }
void BytesMessage::setBytes(const BytesVector &bytes) { _data = bytes; }
void BytesMessage::setBytes(const int8_t *bytes, size_t size) { _data = BytesVector(bytes, bytes + size); }
void BytesMessage::writeBytes(const int8_t *bytes, size_t offset, size_t size) {
  const size_t dataSize = offset + size;
  if (_data.empty()) {
    _data = BytesVector(dataSize, 0);
  }
  auto &vec = _data;
  if (vec.size() < dataSize) {
    vec.reserve(dataSize);
    std::fill(vec.begin() + offset, vec.end(), 0);
  }
  for (size_t i = offset; i < dataSize; ++i) {
    vec[i] = bytes[i - offset];
  }
}
nonstd::span<const int8_t> BytesMessage::readBytes(size_t offset, size_t size) const { return {bytes().begin() + offset, size}; }
void BytesMessage::clearData() { _data.clear(); }
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

Message::Type BytesMessage::type() const { return Message::BYTES_MESSAGE; }

BytesVector BytesMessage::dataAsBytes() const { return _data; }

void BytesMessage::setDataFromBytes(const BytesVector &bytes) { _data = bytes; }
}  // namespace tiny_mq
