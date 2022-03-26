//
// Created by Alexander Bychuk on 22.02.2022.
//
#include "MessageProperty.h"
#include "PocoAnyVisitor.h"

namespace tiny_mq {
bool Properties::hasProperty(const std::string& name) const { return _properties.find(name) != _properties.end(); }
const Properties::PropertyMap& Properties::raw() const { return _properties; }
void Properties::clear() { _properties.clear(); }
Poco::JSON::Object Properties::toJSON() const {
  Poco::JSON::Object properties;
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

  for (const auto& prop : _properties) {
    if (visitor(prop.second)) {
      properties.set(prop.first, value);
    }
  }
  return properties;
}
}  // namespace tiny_mq