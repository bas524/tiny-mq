//
// Created by Alexander Bychuk on 06.04.2022.
//
#include <Poco/JSON/Parser.h>
#include "StreamMessage.h"

namespace tiny_mq {
property::ValueType StreamMessage::nextValueType() const { return _stream.nextValueType(); }
StreamMessage::StreamMessage(Poco::UUID uuid_, Reliability reliability_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
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

Message::Type StreamMessage::type() const { return Message::STREAM_MESSAGE; }

BytesVector StreamMessage::dataAsBytes() const {
  BytesVector result;
  if (!_stream.empty()) {
    auto data = _stream.toJSON();
    std::stringstream ss;
    data.stringify(ss);
    std::string s = ss.str();
    result.reserve(s.size());
    result.assign(s.begin(), s.end());
  }
  return result;
}

void StreamMessage::setDataFromBytes(const BytesVector &bytes) {
  _stream.clear();
  if (!bytes.empty()) {
    Poco::JSON::Parser parser;
    std::string s(bytes.begin(), bytes.end());
    auto result = parser.parse(s);
    auto object = result.extract<Poco::JSON::Object::Ptr>();
    if (!object.isNull()) {
      _stream.fromJSON(*object);
    }
  }
}
}  // namespace tiny_mq
