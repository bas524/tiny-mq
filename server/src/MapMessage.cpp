//
// Created by Alexander Bychuk on 26.03.2022.
//

#include <Poco/JSON/Parser.h>
#include "MapMessage.h"

namespace tiny_mq {
MapMessage::MapMessage(Poco::UUID uuid_, Reliability reliability_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
bool MapMessage::has(const std::string& name) const { return _bodyProps.hasProperty(name); }
void MapMessage::clearData() { _bodyProps.clear(); }
Poco::JSON::Object MapMessage::toJSON() const {
  auto json = propertiesToJSON();
  json.set("data", _bodyProps.toJSON());
  return json;
}
Message::Ptr MapMessage::copy() const { return tiny_mq::Message::Ptr(new MapMessage(*this)); }
Message::Type MapMessage::type() const { return Message::MAP_MESSAGE; }

bool MapMessage::empty() const { return _bodyProps.raw().empty(); }
std::vector<std::string> MapMessage::names() const {
  std::vector<std::string> names;
  names.reserve((_bodyProps.raw().size()));
  for (const auto& item : _bodyProps.raw()) {
    names.push_back(item.first);
  }
  return names;
}
property::ValueType MapMessage::valueType(const std::string& name) const { return _bodyProps.propertyValueType(name); }

BytesVector MapMessage::dataAsBytes() const {
  BytesVector result;
  if (!empty()) {
    auto data = _bodyProps.toJSON();
    std::stringstream ss;
    data.stringify(ss);
    std::string s = ss.str();
    result.reserve(s.size());
    result.assign(s.begin(), s.end());
  }
  return result;
}

void MapMessage::setDataFromBytes(const BytesVector& bytes) {
  _bodyProps.clear();
  if (!bytes.empty()) {
    Poco::JSON::Parser parser;
    std::string s(bytes.begin(), bytes.end());
    auto result = parser.parse(s);
    auto object = result.extract<Poco::JSON::Object::Ptr>();
    if (!object.isNull()) {
      _bodyProps.fromJSON(*object);
    }
  }
}

}  // namespace tiny_mq
