//
// Created by Alexander Bychuk on 22.02.2022.
//
#include "MessageProperty.h"
#include "PocoAnyVisitor.h"
#include <limits>

namespace tiny_mq {
namespace {
template <typename T>
Poco::Dynamic::Var encodeValue(const T& value) {
  if constexpr (std::is_floating_point_v<T>) {
    if (std::isnan(value)) return std::string("NaN");
    if (std::isinf(value)) return std::string(value < 0 ? "-Infinity" : "Infinity");
    if (value == 0 && std::signbit(value)) return std::string("-0.0");
  }
  if constexpr (std::is_same_v<T, property::raw_type::java_character>) return static_cast<uint16_t>(value);
  else return value;
}

template <typename T>
T decodeValue(const Poco::JSON::Object& object) {
  if constexpr (std::is_floating_point_v<T>) {
    auto value = object.get("value");
    if (value.isString()) {
      auto text = value.extract<std::string>();
      if (text == "NaN") return std::numeric_limits<T>::quiet_NaN();
      if (text == "-Infinity") return -std::numeric_limits<T>::infinity();
      if (text == "Infinity") return std::numeric_limits<T>::infinity();
      if (text == "-0.0") return -static_cast<T>(0);
    }
  }
  if constexpr (std::is_same_v<T, property::raw_type::java_character>) return object.getValue<uint16_t>("value");
  else return object.getValue<T>("value");
}

template <typename T>
Poco::Nullable<T> decodeNullable(const Poco::JSON::Object& object) {
  if (object.isNull("value")) return {};
  return decodeValue<T>(object);
}
}  // namespace

namespace property {
template <>
struct Value<Bool> {
  constexpr static ValueType Type = BOOLEAN_TYPE;
};

template <>
struct Value<raw_type::boolean> {
  constexpr static ValueType Type = BOOLEAN_TYPE;
};

template <>
struct Value<Byte> {
  constexpr static ValueType Type = BYTE_TYPE;
};
template <>
struct Value<raw_type::byte> {
  constexpr static ValueType Type = BYTE_TYPE;
};

template <>
struct Value<Char> {
  constexpr static ValueType Type = CHAR_TYPE;
};
template <>
struct Value<raw_type::character> {
  constexpr static ValueType Type = CHAR_TYPE;
};

template <>
struct Value<JavaChar> {
  constexpr static ValueType Type = JAVA_CHAR_TYPE;
};
template <>
struct Value<raw_type::java_character> {
  constexpr static ValueType Type = JAVA_CHAR_TYPE;
};

template <>
struct Value<Short> {
  constexpr static ValueType Type = SHORT_TYPE;
};
template <>
struct Value<raw_type::short_integer> {
  constexpr static ValueType Type = SHORT_TYPE;
};

template <>
struct Value<Int> {
  constexpr static ValueType Type = INTEGER_TYPE;
};
template <>
struct Value<raw_type::integer> {
  constexpr static ValueType Type = INTEGER_TYPE;
};

template <>
struct Value<Long> {
  constexpr static ValueType Type = LONG_TYPE;
};
template <>
struct Value<raw_type::long_integer> {
  constexpr static ValueType Type = LONG_TYPE;
};

template <>
struct Value<Float> {
  constexpr static ValueType Type = FLOAT_TYPE;
};
template <>
struct Value<raw_type::floating_point> {
  constexpr static ValueType Type = FLOAT_TYPE;
};

template <>
struct Value<Double> {
  constexpr static ValueType Type = DOUBLE_TYPE;
};
template <>
struct Value<raw_type::double_point> {
  constexpr static ValueType Type = DOUBLE_TYPE;
};

template <>
struct Value<String> {
  constexpr static ValueType Type = STRING_TYPE;
};
template <>
struct Value<raw_type::string> {
  constexpr static ValueType Type = STRING_TYPE;
};

template <>
struct Value<Bytes> {
  constexpr static ValueType Type = BYTE_ARRAY_TYPE;
};
template <>
struct Value<BytesVector> {
  constexpr static ValueType Type = BYTE_ARRAY_TYPE;
};
std::string getValueTypeName(ValueType type) {
  switch (type) {
    case NULL_TYPE:
      return "NULL";
    case BOOLEAN_TYPE:
      return "BOOLEAN";
    case BYTE_TYPE:
      return "BYTE";
    case CHAR_TYPE:
      return "CHAR";
    case SHORT_TYPE:
      return "SHORT";
    case INTEGER_TYPE:
      return "INTEGER";
    case LONG_TYPE:
      return "LONG";
    case DOUBLE_TYPE:
      return "DOUBLE";
    case FLOAT_TYPE:
      return "FLOAT";
    case STRING_TYPE:
      return "STRING";
    case BYTE_ARRAY_TYPE:
      return "BYTE_ARRAY";
    case JAVA_CHAR_TYPE:
      return "JAVA_CHAR";
    case UNKNOWN_TYPE:
      return "UNKNOWN";
  }
  return {"UNKNOWN"};
}
ValueType getValueTypeFromString(const std::string& typeName) {
  if (typeName == "NULL") return NULL_TYPE;
  if (typeName == "BOOLEAN") return BOOLEAN_TYPE;
  if (typeName == "BYTE") return BYTE_TYPE;
  if (typeName == "CHAR") return CHAR_TYPE;
  if (typeName == "SHORT") return SHORT_TYPE;
  if (typeName == "INTEGER") return INTEGER_TYPE;
  if (typeName == "LONG") return LONG_TYPE;
  if (typeName == "DOUBLE") return DOUBLE_TYPE;
  if (typeName == "FLOAT") return FLOAT_TYPE;
  if (typeName == "STRING") return STRING_TYPE;
  if (typeName == "BYTE_ARRAY") return BYTE_ARRAY_TYPE;
  if (typeName == "JAVA_CHAR") return JAVA_CHAR_TYPE;
  return UNKNOWN_TYPE;
}

}  // namespace property

bool Properties::hasProperty(const std::string& name) const { return _properties.find(name) != _properties.end(); }
const Properties::PropertyMap& Properties::raw() const { return _properties; }
void Properties::clear() {
  _properties.clear();
  _extendedStorage = false;
}
Poco::JSON::Object Properties::toJSON() const {
  Poco::JSON::Object properties;
  Poco::JSON::Object value;
  Poco::AnyVisitor visitor;

  auto valueExtractor = [&value](const auto& propValue) {
    value.set("type", property::getValueTypeName(property::getValueType(propValue)));
    if (!propValue.isNull()) {
      value.set("value", encodeValue(propValue.value()));
    } else {
      value.set("value", Poco::Dynamic::Var{});  // serializes as JSON null
    }
  };

  visitor.insertVisitor<property::Bool>(valueExtractor);
  visitor.insertVisitor<property::Char>(valueExtractor);
  visitor.insertVisitor<property::JavaChar>(valueExtractor);
  visitor.insertVisitor<property::Short>(valueExtractor);
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

void Properties::fromJSON(const Poco::JSON::Object& object) {
  auto properties = object.getNames();
  for (auto property : properties) {
    auto propObj = object.getObject(property);
    if (!propObj.isNull()) {
      property::ValueType type = property::getValueTypeFromString(propObj->getValue<property::raw_type::string>("type"));
      switch (type) {
        case property::BOOLEAN_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::boolean>(*propObj));
        } break;
        case property::BYTE_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::byte>(*propObj));
        } break;
        case property::CHAR_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::character>(*propObj));
        } break;
        case property::SHORT_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::short_integer>(*propObj));
        } break;
        case property::INTEGER_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::integer>(*propObj));
        } break;
        case property::LONG_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::long_integer>(*propObj));
        } break;
        case property::STRING_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::string>(*propObj));
        } break;
        case property::FLOAT_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::floating_point>(*propObj));
        } break;
        case property::DOUBLE_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::double_point>(*propObj));
        } break;
        case property::JAVA_CHAR_TYPE: {
          setProperty(std::move(property), decodeNullable<property::raw_type::java_character>(*propObj));
        } break;
        case property::BYTE_ARRAY_TYPE: {
          if (propObj->isNull("value")) {
            setProperty(std::move(property), property::Bytes{});
            break;
          }
          auto arr = propObj->getArray("value");
          if (!arr.isNull()) {
            BytesVector data(arr->size());
            for (size_t i = 0; i < arr->size(); ++i) {
              data[i] = arr->getElement<property::raw_type::byte>(i);
            }
            setProperty(std::move(property), property::Bytes(std::move(data)));
          }
        } break;
        default:
          break;
      }
    }
  }
}

property::ValueType Properties::propertyValueType(const std::string& name) const {
  Poco::AnyVisitor visitor;
  property::ValueType valueType = property::UNKNOWN_TYPE;
  auto typeExtractor = [&valueType](const auto& propertyValue) { valueType = property::getValueType(propertyValue); };

  visitor.insertVisitor<property::Bool>(typeExtractor);
  visitor.insertVisitor<property::Char>(typeExtractor);
  visitor.insertVisitor<property::JavaChar>(typeExtractor);
  visitor.insertVisitor<property::String>(typeExtractor);
  visitor.insertVisitor<property::Short>(typeExtractor);
  visitor.insertVisitor<property::Byte>(typeExtractor);
  visitor.insertVisitor<property::Int>(typeExtractor);
  visitor.insertVisitor<property::Long>(typeExtractor);
  visitor.insertVisitor<property::Float>(typeExtractor);
  visitor.insertVisitor<property::Double>(typeExtractor);
  visitor.insertVisitor<property::Bytes>(typeExtractor);

  visitor(_properties.at(name));

  return valueType;
}

///////////////////////////////////////////////////////////////
property::ValueType PropertiesStream::nextValueType() const {
  auto noffset = offset;
  Poco::AnyVisitor visitor;
  property::ValueType valueType = property::UNKNOWN_TYPE;
  auto typeExtractor = [&valueType](const auto& propertyValue) { valueType = property::getValueType(propertyValue); };

  visitor.insertVisitor<property::raw_type::boolean>(typeExtractor);
  visitor.insertVisitor<property::raw_type::character>(typeExtractor);
  visitor.insertVisitor<property::raw_type::java_character>(typeExtractor);
  visitor.insertVisitor<property::raw_type::string>(typeExtractor);
  visitor.insertVisitor<property::raw_type::short_integer>(typeExtractor);
  visitor.insertVisitor<property::raw_type::byte>(typeExtractor);
  visitor.insertVisitor<property::raw_type::integer>(typeExtractor);
  visitor.insertVisitor<property::raw_type::long_integer>(typeExtractor);
  visitor.insertVisitor<property::raw_type::floating_point>(typeExtractor);
  visitor.insertVisitor<property::raw_type::double_point>(typeExtractor);
  visitor.insertVisitor<BytesVector>(typeExtractor);

  if (noffset == _properties.end() || noffset->empty()) return property::NULL_TYPE;
  visitor(*noffset);

  return valueType;
}
void PropertiesStream::readNull() const {
  if (offset == _properties.end() || !offset->empty()) throw Poco::BadCastException("Stream item is not null");
  ++offset;
}
void PropertiesStream::clear() {
  _extendedStorage = false;
  _properties.clear();
  offset = _properties.begin();
}
Poco::JSON::Object PropertiesStream::toJSON() const {
  Poco::JSON::Object properties;
  Poco::JSON::Array stream;
  Poco::JSON::Object value;
  Poco::AnyVisitor visitor;

  auto valueExtractor = [&value](const auto& propValue) {
    value.set("type", property::getValueTypeName(property::getValueType(propValue)));
    value.set("value", encodeValue(propValue));
  };

  visitor.insertVisitor<property::raw_type::boolean>(valueExtractor);
  visitor.insertVisitor<property::raw_type::character>(valueExtractor);
  visitor.insertVisitor<property::raw_type::java_character>(valueExtractor);
  visitor.insertVisitor<property::raw_type::string>(valueExtractor);
  visitor.insertVisitor<property::raw_type::short_integer>(valueExtractor);
  visitor.insertVisitor<property::raw_type::byte>(valueExtractor);
  visitor.insertVisitor<property::raw_type::integer>(valueExtractor);
  visitor.insertVisitor<property::raw_type::long_integer>(valueExtractor);
  visitor.insertVisitor<property::raw_type::floating_point>(valueExtractor);
  visitor.insertVisitor<property::raw_type::double_point>(valueExtractor);
  visitor.insertVisitor<BytesVector>(valueExtractor);

  for (const auto& prop : _properties) {
    if (prop.empty()) {
      Poco::JSON::Object nullValue;
      nullValue.set("type", "NULL");
      nullValue.set("value", Poco::Dynamic::Var{});
      stream.add(nullValue);
    } else if (visitor(prop)) {
      stream.add(value);
    }
  }
  properties.set("stream", stream);
  return properties;
}

void PropertiesStream::fromJSON(const Poco::JSON::Object& object) {
  auto stream = object.getArray("stream");
  if (!stream.isNull()) {
    for (size_t i = 0; i < stream->size(); ++i) {
      auto o = stream->getObject(i);
      property::ValueType type = property::getValueTypeFromString(o->getValue<property::raw_type::string>("type"));
      switch (type) {
        case property::BOOLEAN_TYPE: {
          write(decodeValue<property::raw_type::boolean>(*o));
        } break;
        case property::BYTE_TYPE: {
          write(decodeValue<property::raw_type::byte>(*o));
        } break;
        case property::CHAR_TYPE: {
          write(decodeValue<property::raw_type::character>(*o));
        } break;
        case property::SHORT_TYPE: {
          write(decodeValue<property::raw_type::short_integer>(*o));
        } break;
        case property::INTEGER_TYPE: {
          write(decodeValue<property::raw_type::integer>(*o));
        } break;
        case property::LONG_TYPE: {
          write(decodeValue<property::raw_type::long_integer>(*o));
        } break;
        case property::STRING_TYPE: {
          write(decodeValue<property::raw_type::string>(*o));
        } break;
        case property::FLOAT_TYPE: {
          write(decodeValue<property::raw_type::floating_point>(*o));
        } break;
        case property::DOUBLE_TYPE: {
          write(decodeValue<property::raw_type::double_point>(*o));
        } break;
        case property::NULL_TYPE:
          writeNull();
          break;
        case property::JAVA_CHAR_TYPE: {
          write(decodeValue<property::raw_type::java_character>(*o));
        } break;
        case property::BYTE_ARRAY_TYPE: {
          auto arr = o->getArray("value");
          if (!arr.isNull()) {
            BytesVector data(arr->size());
            for (size_t j = 0; j < arr->size(); ++j) {
              data[j] = arr->getElement<property::raw_type::byte>(j);
            }
            write(data);
          }
        } break;
        default:
          break;
      }
    }
  }
}
bool PropertiesStream::empty() const { return _properties.empty(); }
void PropertiesStream::reset() { offset = _properties.begin(); }
bool PropertiesStream::eof() const { return offset == _properties.end(); }
}  // namespace tiny_mq
