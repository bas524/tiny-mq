//
// Created by Alexander Bychuk on 11.02.2022.
//

#ifndef TINY_MQ__MESSAGEPROPERTY_H_
#define TINY_MQ__MESSAGEPROPERTY_H_

#include <Poco/Nullable.h>
#include <Poco/Any.h>
#include <type_traits>
#include <map>
#include <Poco/JSON/Object.h>
namespace tiny_mq {

using BytesVector = std::vector<int8_t>;

namespace property {

template <typename T>
constexpr bool IsBoolT = std::is_same<T, bool>::value;

template <typename T>
constexpr bool IsCharT = std::is_same<T, char>::value;

template <typename T>
constexpr bool IsStringT = std::is_same<T, std::string>::value;

template <typename T>
constexpr bool IsByteT = std::is_same<T, int8_t>::value;

template <typename T>
constexpr bool IsShortT = std::is_same<T, int16_t>::value;

template <typename T>
constexpr bool IsIntT = std::is_same<T, int32_t>::value;

template <typename T>
constexpr bool IsLongT = std::is_same<T, int64_t>::value;

template <typename T>
constexpr bool IsFloatT = std::is_same<T, float>::value;

template <typename T>
constexpr bool IsDoubleT = std::is_same<T, double>::value;

template <typename T>
constexpr bool IsBytesT = std::is_same<T, std::vector<int8_t>>::value;

template <typename T>
constexpr bool IsValidType =
    IsBoolT<T> || IsCharT<T> || IsStringT<T> || IsByteT<T> || IsShortT<T> || IsIntT<T> || IsLongT<T> || IsFloatT<T> || IsDoubleT<T> || IsBytesT<T>;

using Bool = Poco::Nullable<bool>;
using Char = Poco::Nullable<char>;
using String = Poco::Nullable<std::string>;
using Byte = Poco::Nullable<int8_t>;
using Short = Poco::Nullable<int16_t>;
using Int = Poco::Nullable<int32_t>;
using Long = Poco::Nullable<int64_t>;
using Float = Poco::Nullable<float>;
using Double = Poco::Nullable<double>;
using Bytes = Poco::Nullable<BytesVector>;

template <typename T>
constexpr bool IsBoolProperty = std::is_same<T, Bool>::value;

template <typename T>
constexpr bool IsCharProperty = std::is_same<T, Char>::value;

template <typename T>
constexpr bool IsStringProperty = std::is_same<T, String>::value;

template <typename T>
constexpr bool IsByteProperty = std::is_same<T, Byte>::value;

template <typename T>
constexpr bool IsShortProperty = std::is_same<T, Short>::value;

template <typename T>
constexpr bool IsIntProperty = std::is_same<T, Int>::value;

template <typename T>
constexpr bool IsLongProperty = std::is_same<T, Long>::value;

template <typename T>
constexpr bool IsFloatProperty = std::is_same<T, Float>::value;

template <typename T>
constexpr bool IsDoubleProperty = std::is_same<T, Double>::value;

template <typename T>
constexpr bool IsBytesProperty = std::is_same<T, Bytes>::value;

template <typename T>
constexpr bool IsValidProperty = IsBoolProperty<T> || IsCharProperty<T> || IsStringProperty<T> || IsByteProperty<T> || IsShortProperty<T> ||
                                 IsIntProperty<T> || IsLongProperty<T> || IsFloatProperty<T> || IsDoubleProperty<T> || IsBytesProperty<T>;

}  // namespace property

class Properties {
  using PropertyMap = std::map<std::string, Poco::Any>;
  PropertyMap _properties;

 public:
  template <typename T>
  using TypeIsProperty = std::enable_if_t<property::IsValidProperty<T>, int>;

  template <typename T, TypeIsProperty<T> = 0>
  void setProperty(const std::string &name, const T &value) {
    _properties.template emplace(name, Poco::Any(value));
  }
  template <typename T, TypeIsProperty<T> = 0>
  const T &getProperty(const std::string &name) const {
    return Poco::RefAnyCast<T>(_properties.at(name));
  }
  bool hasProperty(const std::string &name) const;
  const PropertyMap &raw() const;
  void clear();
  Poco::JSON::Object toJSON() const;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__MESSAGEPROPERTY_H_
