//
// ObjectMessage implementation. Self-contained data layout in dataAsBytes():
//   [uint32 LE className length][className bytes][body bytes]
// so it round-trips through Message::toBytes/fromBytes and storage without
// touching the shared header format.
//

#include "ObjectMessage.h"
#include <Poco/Base64Encoder.h>
#include <cstring>
#include <sstream>

namespace tiny_mq {

ObjectMessage::ObjectMessage(Poco::UUID uuid_, Reliability reliability_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}

ObjectMessage::ObjectMessage(Poco::UUID uuid_, BytesVector body_, std::string className_, Reliability reliability_)
    : _body(std::move(body_)), _className(std::move(className_)) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}

void ObjectMessage::setBody(BytesVector serialized, std::string className) {
  _body = std::move(serialized);
  _className = std::move(className);
}

const BytesVector &ObjectMessage::body() const { return _body; }
const std::string &ObjectMessage::className() const { return _className; }

void ObjectMessage::clearData() {
  _body.clear();
  _className.clear();
}

Poco::JSON::Object ObjectMessage::toJSON() const {
  Poco::JSON::Object json = propertiesToJSON();
  json.set("className", _className);
  std::ostringstream ostr;
  Poco::Base64Encoder encoder(ostr);
  for (auto c : _body) {
    encoder << c;
  }
  encoder.close();
  json.set("data", ostr.str());
  return json;
}

Message::Ptr ObjectMessage::copy() const { return Message::Ptr(new ObjectMessage(*this)); }

Message::Type ObjectMessage::type() const { return Message::OBJECT_MESSAGE; }

BytesVector ObjectMessage::dataAsBytes() const {
  auto nameLen = static_cast<Poco::UInt32>(_className.size());
  BytesVector out(sizeof(nameLen) + nameLen + _body.size());
  size_t off = 0;
  std::memcpy(out.data() + off, &nameLen, sizeof(nameLen)); off += sizeof(nameLen);
  if (nameLen > 0) { std::memcpy(out.data() + off, _className.data(), nameLen); off += nameLen; }
  if (!_body.empty()) { std::memcpy(out.data() + off, _body.data(), _body.size()); }
  return out;
}

void ObjectMessage::setDataFromBytes(const BytesVector &bytes) {
  _className.clear();
  _body.clear();
  if (bytes.size() < sizeof(Poco::UInt32)) return;
  size_t off = 0;
  Poco::UInt32 nameLen = 0;
  std::memcpy(&nameLen, bytes.data() + off, sizeof(nameLen)); off += sizeof(nameLen);
  if (off + nameLen > bytes.size()) return;  // truncated
  _className.assign(reinterpret_cast<const char *>(bytes.data() + off), nameLen); off += nameLen;
  if (off < bytes.size()) {
    _body.assign(bytes.begin() + static_cast<ptrdiff_t>(off), bytes.end());
  }
}

}  // namespace tiny_mq
