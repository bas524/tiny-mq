//
// Created by Alexander Bychuk on 11.02.2022.
//

#ifndef TINY_MQ__MESSAGEPROPERTY_H_
#define TINY_MQ__MESSAGEPROPERTY_H_

#include <Poco/Nullable.h>
#include <Poco/Any.h>
#include <type_traits>
#include <map>
namespace tiny_mq {
namespace property {

template <typename T>
constexpr bool IsBoolT = std::is_same<T, bool>::value;

template <typename T>
constexpr bool IsCharT = std::is_same<T, char>::value;

template <typename T>
constexpr bool IsStringT = std::is_same<T, std::string>::value;

template <typename T>
constexpr bool IsByteT = std::is_same<T, uint8_t>::value;

template <typename T>
constexpr bool IsIntT = std::is_same<T, int32_t>::value;

template <typename T>
constexpr bool IsLongT = std::is_same<T, int64_t>::value;

template <typename T>
constexpr bool IsFloatT = std::is_same<T, float>::value;

template <typename T>
constexpr bool IsDoubleT = std::is_same<T, double>::value;

template <typename T>
constexpr bool IsBytesT = std::is_same<T, std::vector<uint8_t>>::value;

template <typename T>
constexpr bool IsObjectT = std::is_same<T, std::vector<int16_t>>::value;

template <typename T>
constexpr bool IsValidType =
    IsBoolT<T> || IsCharT<T> || IsStringT<T> || IsByteT<T> || IsIntT<T> || IsLongT<T> || IsFloatT<T> || IsDoubleT<T> || IsBytesT<T> || IsObjectT<T>;

using Bool = Poco::Nullable<bool>;
using Char = Poco::Nullable<char>;
using String = Poco::Nullable<std::string>;
using Byte = Poco::Nullable<uint8_t>;
using Int = Poco::Nullable<int32_t>;
using Long = Poco::Nullable<int64_t>;
using Float = Poco::Nullable<float>;
using Double = Poco::Nullable<double>;
using Bytes = Poco::Nullable<std::vector<uint8_t>>;
using Object = Poco::Nullable<std::vector<int16_t>>;
}  // namespace property

class Properties {
  using PropertyMap = std::map<std::string, Poco::Any>;
  PropertyMap _properties;

 public:
  template <typename T>
  using TypeIsProperty = std::enable_if_t<property::IsValidType<T>, int>;

  template <typename T, TypeIsProperty<T> = 0>
  void setProperty(const std::string &name, const Poco::Nullable<T> &value) {
    _properties.template emplace(name, Poco::Any(value));
  }
  template <typename T, TypeIsProperty<T> = 0>
  const Poco::Nullable<T> &getProperty(const std::string &name) const {
    return Poco::RefAnyCast<Poco::Nullable<T>>(_properties.at(name));
  }
  bool hasProperty(const std::string &name) const;
  const PropertyMap &raw() const;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__MESSAGEPROPERTY_H_
