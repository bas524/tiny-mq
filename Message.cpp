//
// Created by Alexander Bychuk on 10.11.2021.
//
#include "Message.h"
#include <Poco/Dynamic/Var.h>
#include "PocoAnyVisitor.h"

namespace tiny_mq {

bool Message::isPersistent() const { return reliability == PERSISTENT; }
bool Message::hasProperty(const std::string& name) const { return _properties.hasProperty(name); }
void Message::clearData() { data = {}; }
Poco::JSON::Object Message::propertytoJSON() const {
  Poco::JSON::Object json;
  Poco::JSON::Object properties;
  Poco::JSON::Object persistInfo;
  Poco::Dynamic::Var value;
  Poco::AnyVisitor visitor;

  auto valueExtractor = [&value](const auto& propValue) {
    if (!propValue.isNull()) {
      value = propValue.value();
    } else {
      value = {};
    }
  };

  visitor.insertVisitor<property::Bool>(valueExtractor);
  visitor.insertVisitor<property::Char>(valueExtractor);
  visitor.insertVisitor<property::String>(valueExtractor);
  visitor.insertVisitor<property::Byte>(valueExtractor);
  visitor.insertVisitor<property::Int>(valueExtractor);
  visitor.insertVisitor<property::Long>(valueExtractor);
  visitor.insertVisitor<property::Float>(valueExtractor);
  visitor.insertVisitor<property::Double>(valueExtractor);
  visitor.insertVisitor<property::Bytes>(valueExtractor);
  visitor.insertVisitor<property::Object>(valueExtractor);

  for (const auto& prop : _properties.raw()) {
    if (visitor(prop.second)) {
      properties.set(prop.first, value);
    }
  }
  json.set("properties", properties);
  json.set("number", _number);
  json.set("uuid", uuid);
  json.set("reliability", (reliability == NOT_PERSISTENT) ? "NOT_PERSISTENT" : "PERSISTENT");
  persistInfo.set("fileFromName", persistentInfo.fileFromName);
  persistInfo.set("fileToName", persistentInfo.fileToName);
  json.set("persistentInfo", persistInfo);
  return json;
}
}  // namespace tiny_mq