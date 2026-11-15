//
// Created by Alexander Bychuk on 11.02.2022.
//

#ifndef TINY_MQ__MESSAGEPROPERTY_H_
#define TINY_MQ__MESSAGEPROPERTY_H_

#include <Poco/Nullable.h>
#include <Poco/Any.h>
#include <type_traits>
#include <cmath>
#include <unordered_map>
#include <Poco/JSON/Object.h>
namespace tiny_mq {

using BytesVector = std::vector<int8_t>;

namespace property {
namespace raw_type {
using boolean = bool;
using character = char;
using java_character = char16_t;
using byte = int8_t;
using short_integer = int16_t;
using integer = int32_t;
using long_integer = int64_t;
using floating_point = float;
using double_point = double;
using string = std::string;
}  // namespace raw_type

template <typename T>
constexpr bool IsBoolT = std::is_same<T, raw_type::boolean>::value;

template <typename T>
constexpr bool IsCharT = std::is_same<T, raw_type::character>::value;

template <typename T>
constexpr bool IsStringT = std::is_same<T, raw_type::string>::value;

template <typename T>
constexpr bool IsByteT = std::is_same<T, raw_type::byte>::value;

template <typename T>
constexpr bool IsShortT = std::is_same<T, raw_type::short_integer>::value;

template <typename T>
constexpr bool IsIntT = std::is_same<T, raw_type::integer>::value;

template <typename T>
constexpr bool IsLongT = std::is_same<T, raw_type::long_integer>::value;

template <typename T>
constexpr bool IsFloatT = std::is_same<T, raw_type::floating_point>::value;

template <typename T>
constexpr bool IsDoubleT = std::is_same<T, raw_type::double_point>::value;

template <typename T>
constexpr bool IsBytesT = std::is_same<T, BytesVector>::value;

template <typename T>
constexpr bool IsValidType =
    IsBoolT<T> || IsCharT<T> || std::is_same_v<T, raw_type::java_character> || IsStringT<T> || IsByteT<T> || IsShortT<T> || IsIntT<T> || IsLongT<T> || IsFloatT<T> || IsDoubleT<T> || IsBytesT<T>;

using Bool = Poco::Nullable<raw_type::boolean>;
using Char = Poco::Nullable<raw_type::character>;
using JavaChar = Poco::Nullable<raw_type::java_character>;
using String = Poco::Nullable<raw_type::string>;
using Byte = Poco::Nullable<raw_type::byte>;
using Short = Poco::Nullable<raw_type::short_integer>;
using Int = Poco::Nullable<raw_type::integer>;
using Long = Poco::Nullable<raw_type::long_integer>;
using Float = Poco::Nullable<raw_type::floating_point>;
using Double = Poco::Nullable<raw_type::double_point>;
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
constexpr bool IsValidProperty = IsBoolProperty<T> || IsCharProperty<T> || std::is_same_v<T, JavaChar> || IsStringProperty<T> || IsByteProperty<T> || IsShortProperty<T> ||
                                 IsIntProperty<T> || IsLongProperty<T> || IsFloatProperty<T> || IsDoubleProperty<T> || IsBytesProperty<T>;

enum ValueType {
  NULL_TYPE = 0,
  BOOLEAN_TYPE = 1,
  BYTE_TYPE = 2,
  CHAR_TYPE = 3,
  SHORT_TYPE = 4,
  INTEGER_TYPE = 5,
  LONG_TYPE = 6,
  DOUBLE_TYPE = 7,
  FLOAT_TYPE = 8,
  STRING_TYPE = 9,
  BYTE_ARRAY_TYPE = 10,
  UNKNOWN_TYPE = 11,
  JAVA_CHAR_TYPE = 12
};

template <typename T>
struct Value {
  constexpr static ValueType Type = UNKNOWN_TYPE;
};

template <typename T>
ValueType getValueType(const T &) {
  return property::Value<T>::Type;
}

std::string getValueTypeName(ValueType type);
ValueType getValueTypeFromString(const std::string &typeName);
}  // namespace property

using PropertyValue = Poco::Any;

class Properties {
  using PropertyMap = std::unordered_map<std::string, PropertyValue>;
  PropertyMap _properties;
  bool _extendedStorage{false};

 public:
  template <typename T>
  using TypeIsProperty = std::enable_if_t<property::IsValidProperty<T>, int>;

  template <typename T, TypeIsProperty<T> = 0>
  void setProperty(std::string name, T value) {
    _extendedStorage |= value.isNull() || std::is_same_v<T, property::JavaChar>;
    if constexpr (property::IsFloatProperty<T> || property::IsDoubleProperty<T>) {
      if (!value.isNull()) _extendedStorage |= !std::isfinite(value.value()) ||
          (value.value() == 0 && std::signbit(value.value()));
    }
    if constexpr (property::IsBytesProperty<T>) {
      if (!value.isNull()) _extendedStorage |= value.value().empty();
    }
    _properties.insert_or_assign(std::move(name), Poco::Any(std::move(value)));
  }

  template <typename T, TypeIsProperty<T> = 0>
  const T &property(const std::string &name) const {
    return Poco::RefAnyCast<T>(_properties.at(name));
  }
  const PropertyValue* find(const std::string& name) const {
    auto it = _properties.find(name);
    return it == _properties.end() ? nullptr : &it->second;
  }
  bool hasProperty(const std::string &name) const;
  property::ValueType propertyValueType(const std::string &name) const;
  const PropertyMap &raw() const;
  bool needsExtendedStorage() const { return _extendedStorage; }
  void clear();
  Poco::JSON::Object toJSON() const;
  void fromJSON(const Poco::JSON::Object &object);
};

class PropertiesStream {
  using Stream = std::list<Poco::Any>;
  Stream _properties;
  bool _extendedStorage{false};
  mutable Stream::iterator offset = _properties.begin();

 public:
  template <typename T>
  using TypeAllowed = std::enable_if_t<property::IsValidType<T>, int>;

  PropertiesStream() = default;
  PropertiesStream(const PropertiesStream &other) : _properties(other._properties), _extendedStorage(other._extendedStorage) {}
  PropertiesStream &operator=(const PropertiesStream &other) {
    if (this != &other) {
      _properties = other._properties;
      _extendedStorage = other._extendedStorage;
      reset();
    }
    return *this;
  }

  template <typename T, TypeAllowed<T> = 0>
  void write(const T &value) {
    _extendedStorage |= std::is_same_v<T, property::raw_type::java_character>;
    if constexpr (std::is_floating_point_v<T>) {
      _extendedStorage |= !std::isfinite(value) || (value == 0 && std::signbit(value));
    }
    if constexpr (property::IsBytesT<T>) _extendedStorage |= value.empty();
    _properties.push_back(Poco::Any(value));
  }
  template <typename T, TypeAllowed<T> = 0>
  const T &nextValue() const {
    const auto &result = Poco::RefAnyCast<T>(*offset);
    offset = std::next(offset);
    return result;
  }
  void writeNull() {
    _extendedStorage = true;
    _properties.emplace_back();
  }
  void readNull() const;
  bool needsExtendedStorage() const { return _extendedStorage; }
  property::ValueType nextValueType() const;
  void reset();
  bool eof() const;
  bool empty() const;
  void clear();
  Poco::JSON::Object toJSON() const;
  void fromJSON(const Poco::JSON::Object &object);
};
}  // namespace tiny_mq
#endif  // TINY_MQ__MESSAGEPROPERTY_H_
