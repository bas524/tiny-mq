//
// Created by Alexander Bychuk on 22.02.2022.
//
#include "MessageProperty.h"
namespace tiny_mq {
bool Properties::hasProperty(const std::string& name) const { return _properties.find(name) != _properties.end(); }
const Properties::PropertyMap& Properties::raw() const { return _properties; }
}  // namespace tiny_mq