//
// Created by Alexander Bychuk on 10.11.2021.
//
#include "Message.h"
#include <Poco/Dynamic/Var.h>
#include "PocoAnyVisitor.h"

namespace tiny_mq {

bool Message::isPersistent() const { return reliability == PERSISTENT; }
bool Message::hasProperty(const std::string& name) const { return _properties.hasProperty(name); }
property::ValueType Message::propertyValueType(const std::string& name) const { return _properties.propertyValueType(name); }
void Message::clearData() { data = {}; }
Poco::JSON::Object Message::propertiesToJSON() const {
  Poco::JSON::Object json;
  Poco::JSON::Object properties = _properties.toJSON();

  json.set("properties", properties);
  json.set("number", _number);
  json.set("uuid", uuid);
  json.set("reliability", (reliability == NOT_PERSISTENT) ? "NOT_PERSISTENT" : "PERSISTENT");

  Poco::JSON::Object persistInfo;
  persistInfo.set("fileFromName", persistentInfo.fileFromName);
  persistInfo.set("fileToName", persistentInfo.fileToName);
  json.set("persistentInfo", persistInfo);
  return json;
}
}  // namespace tiny_mq