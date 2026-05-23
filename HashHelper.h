#ifndef HASHHELPER_H
#define HASHHELPER_H

#include <Poco/UUID.h>
#include <Poco/Hash.h>
#include "parallel_hashmap/phmap.h"
#include <functional>

namespace std {
template <>
struct hash<Poco::UUID> {
  [[nodiscard]] static size_t _hash(const Poco::UUID &val) { return std::hash<std::string>{}(val.toString()); }
  
  inline size_t operator()(const Poco::UUID &val) const { return _hash(val); }
};
}  // namespace std

namespace Poco {
template <>
struct Hash<Poco::UUID> {
  [[nodiscard]] static size_t _hash(const Poco::UUID &val) { return std::hash<std::string>{}(val.toString()); }
  
  inline size_t operator()(const Poco::UUID &val) const { return _hash(val); }
};
}  // namespace Poco

namespace phmap {
template <>
struct Hash<Poco::UUID> {
  [[nodiscard]] static size_t _hash(const Poco::UUID &val) { return std::hash<std::string>{}(val.toString()); }

  inline size_t operator()(const Poco::UUID &val) const { return _hash(val); }
};
}  // namespace phmap

#endif // HASHHELPER_H
