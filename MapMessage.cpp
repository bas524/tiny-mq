//
// Created by Alexander Bychuk on 26.03.2022.
//

#include "MapMessage.h"

namespace tiny_mq {
bool MapMessage::has(const std::string& name) const { return _bodyProps.hasProperty(name); }
void MapMessage::clearData() { _bodyProps.clear(); }
Poco::JSON::Object MapMessage::toJSON() const {
  auto json = propertiesToJSON();
  json.set("data", _bodyProps.toJSON());
  return json;
}
Message::Ptr MapMessage::copy() const { return tiny_mq::Message::Ptr(new MapMessage(*this)); }
bool MapMessage::empty() const { return _bodyProps.raw().empty(); }
std::vector<std::string> MapMessage::names() const {
  std::vector<std::string> names;
  names.reserve((_bodyProps.raw().size()));
  for (const auto& item : _bodyProps.raw()) {
    names.push_back(item.first);
  }
  return names;
}
}  // namespace tiny_mq