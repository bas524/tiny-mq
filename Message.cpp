//
// Created by Alexander Bychuk on 10.11.2021.
//
#include "Message.h"
#include <Poco/Dynamic/Var.h>
#include <Poco/JSON/Parser.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <sstream>
#include <limits>
#include <cstring>
#include <cstdint>

namespace tiny_mq {

bool Message::isPersistent() const { return reliability == PERSISTENT; }

bool Message::isExpired(int64_t nowMs) const {
  // JMSExpiration == 0 means "never expires" (JMS 2.0 § 3.4.9).
  return jmsHeaders.expiration != 0 && nowMs >= jmsHeaders.expiration;
}

void Message::patchCachedDeliveryTime(int64_t deliveryTime) {
  // Offset within _cachedStorageBytes (= [1-byte type prefix][toBytes() 0x02
  // payload]): 1 type + 1 magic + 8 number + 16 uuid + 1 reliability +
  // 8 timestamp + 8 expiration = 43 — matches the deliveryTime slot written by
  // toBytes()/read by fromBytes() above.
  constexpr size_t kOffset = 1                    // type byte
                            + 1                    // magic
                            + sizeof(int64_t)       // number
                            + 16                    // uuid
                            + 1                     // reliability
                            + sizeof(int64_t)       // timestamp
                            + sizeof(int64_t);      // expiration  (total = 43)
  constexpr size_t kSize = sizeof(int64_t);
  if (_cachedStorageBytes.size() < kOffset + kSize) return;
  if (static_cast<uint8_t>(_cachedStorageBytes[1]) != 0x02) return;  // pre-header format: nothing to patch
  std::memcpy(_cachedStorageBytes.data() + kOffset, &deliveryTime, kSize);
}

void Message::refreshCachedStorageBytes() {
  if (!isPersistent()) return;
  auto bytesData = toBytes();
  std::vector<char> data;
  data.reserve(1 + bytesData.size());
  data.push_back(static_cast<char>(type()));
  data.insert(data.end(), bytesData.begin(), bytesData.end());
  _cachedStorageBytes = std::move(data);
}

int64_t Message::number() const { return _number; }
bool Message::hasProperty(const std::string& name) const { return _properties.hasProperty(name); }
property::ValueType Message::propertyValueType(const std::string& name) const { return _properties.propertyValueType(name); }

BytesVector Message::propertiesAsBytes() const {
  BytesVector result;
  if (!_properties.raw().empty()) {
    auto props = propertiesToJSON();
    std::stringstream ss;
    props.stringify(ss);
    std::string s = ss.str();
    result.assign(s.begin(), s.end());
  }
  return result;
}

namespace {
// Helper: write a length-prefixed string (uint32 LE length + bytes).
void writeString(BytesVector& out, size_t& off, const std::string& s) {
  auto len = static_cast<Poco::UInt32>(s.size());
  memcpy(out.data() + off, &len, sizeof(len)); off += sizeof(len);
  if (len > 0) { memcpy(out.data() + off, s.data(), len); off += len; }
}

// Helper: read a length-prefixed string. Returns false on truncation.
bool readString(const BytesVector& in, size_t& off, std::string& out) {
  if (off + sizeof(Poco::UInt32) > in.size()) return false;
  Poco::UInt32 len = 0;
  memcpy(&len, in.data() + off, sizeof(len)); off += sizeof(len);
  if (off + len > in.size()) return false;
  out.assign(reinterpret_cast<const char*>(in.data() + off), len);
  off += len;
  return true;
}
}  // namespace

// Binary wire format:
// Magic 0x01 (legacy, pre-headers):
//   [1]  uint8_t  magic
//   [8]  int64_t  message number
//   [16] bytes    UUID
//   [1]  uint8_t  reliability (0=NOT_PERSISTENT, 1=PERSISTENT)
//   [4]  uint32_t properties JSON byte length
//   [n]  bytes    properties JSON
//   [m]  bytes    payload data
//
// Magic 0x02 (JMS 2.0 headers):
//   [1]  uint8_t  magic
//   [8]  int64_t  message number
//   [16] bytes    UUID
//   [1]  uint8_t  reliability
//   --- JMS headers block (fixed 28 bytes) ---
//   [8]  int64_t  timestamp
//   [8]  int64_t  expiration
//   [8]  int64_t  deliveryTime
//   [4]  int32_t  priority
//   [4]  int32_t  deliveryCount
//   [1]  uint8_t  flags (bit 0 = redelivered)
//   --- JMS header strings ---
//   [4+n] string  messageId
//   [4+n] string  replyTo
//   [4+n] string  correlationId
//   [4+n] string  type
//   --- properties + data ---
//   [4]  uint32_t properties JSON byte length
//   [n]  bytes    properties JSON
//   [m]  bytes    payload data
//
// Records older than 0x01 use the legacy JSON path in fromBytes().
BytesVector Message::toBytes() const {
  BytesVector data = dataAsBytes();

  std::string propsJson;
  if (!_properties.raw().empty()) {
    Poco::JSON::Object propsObj = _properties.toJSON();
    std::ostringstream ss;
    propsObj.stringify(ss);
    propsJson = ss.str();
  }

  auto propsLen = static_cast<Poco::UInt32>(propsJson.size());
  constexpr size_t kFixed   = 1 + sizeof(int64_t) + 16 + 1;
  constexpr size_t kHdrNums = sizeof(int64_t) * 3 + sizeof(int32_t) * 2 + 1;  // 28
  const size_t kHdrStrs = 4 * sizeof(Poco::UInt32)
                          + jmsHeaders.messageId.size()
                          + jmsHeaders.replyTo.size()
                          + jmsHeaders.correlationId.size()
                          + jmsHeaders.type.size();
  BytesVector result(kFixed + kHdrNums + kHdrStrs + sizeof(Poco::UInt32) + propsLen + data.size());

  size_t off = 0;
  result[off++] = static_cast<int8_t>(0x02);                              // magic
  memcpy(result.data() + off, &_number, sizeof(_number)); off += sizeof(_number);
  char uuidBuf[16]; uuid.copyTo(uuidBuf);
  memcpy(result.data() + off, uuidBuf, 16); off += 16;
  result[off++] = static_cast<int8_t>(reliability);

  // Headers block (numeric)
  memcpy(result.data() + off, &jmsHeaders.timestamp, sizeof(int64_t));     off += sizeof(int64_t);
  memcpy(result.data() + off, &jmsHeaders.expiration, sizeof(int64_t));    off += sizeof(int64_t);
  memcpy(result.data() + off, &jmsHeaders.deliveryTime, sizeof(int64_t));  off += sizeof(int64_t);
  memcpy(result.data() + off, &jmsHeaders.priority, sizeof(int32_t));      off += sizeof(int32_t);
  memcpy(result.data() + off, &jmsHeaders.deliveryCount, sizeof(int32_t)); off += sizeof(int32_t);
  result[off++] = static_cast<int8_t>(jmsHeaders.redelivered ? 1 : 0);

  // Headers block (strings)
  writeString(result, off, jmsHeaders.messageId);
  writeString(result, off, jmsHeaders.replyTo);
  writeString(result, off, jmsHeaders.correlationId);
  writeString(result, off, jmsHeaders.type);

  // Properties + data
  memcpy(result.data() + off, &propsLen, sizeof(propsLen)); off += sizeof(propsLen);
  if (propsLen > 0) { memcpy(result.data() + off, propsJson.data(), propsLen); off += propsLen; }
  if (!data.empty()) { memcpy(result.data() + off, data.data(), data.size()); }

  return result;
}

void Message::fromBytes(const BytesVector& bytes) {
  if (bytes.empty()) return;

  const uint8_t magic = static_cast<uint8_t>(bytes[0]);
  if (magic == 0x01 || magic == 0x02) {
    constexpr size_t kFixed = 1 + sizeof(int64_t) + 16 + 1;
    if (bytes.size() < kFixed) return;

    size_t off = 1;  // skip magic
    memcpy(&_number, bytes.data() + off, sizeof(_number)); off += sizeof(_number);
    uuid.copyFrom(reinterpret_cast<const char*>(bytes.data() + off)); off += 16;
    reliability = (static_cast<uint8_t>(bytes[off++]) != 0) ? PERSISTENT : NOT_PERSISTENT;

    if (magic == 0x02) {
      constexpr size_t kHdrNums = sizeof(int64_t) * 3 + sizeof(int32_t) * 2 + 1;
      if (off + kHdrNums > bytes.size()) return;
      memcpy(&jmsHeaders.timestamp, bytes.data() + off, sizeof(int64_t));     off += sizeof(int64_t);
      memcpy(&jmsHeaders.expiration, bytes.data() + off, sizeof(int64_t));    off += sizeof(int64_t);
      memcpy(&jmsHeaders.deliveryTime, bytes.data() + off, sizeof(int64_t));  off += sizeof(int64_t);
      memcpy(&jmsHeaders.priority, bytes.data() + off, sizeof(int32_t));      off += sizeof(int32_t);
      memcpy(&jmsHeaders.deliveryCount, bytes.data() + off, sizeof(int32_t)); off += sizeof(int32_t);
      jmsHeaders.redelivered = (static_cast<uint8_t>(bytes[off++]) != 0);

      if (!readString(bytes, off, jmsHeaders.messageId)) return;
      if (!readString(bytes, off, jmsHeaders.replyTo)) return;
      if (!readString(bytes, off, jmsHeaders.correlationId)) return;
      if (!readString(bytes, off, jmsHeaders.type)) return;
    } else {
      // 0x01 — leave jmsHeaders at defaults.
      jmsHeaders = Headers{};
    }

    if (off + sizeof(Poco::UInt32) > bytes.size()) return;
    Poco::UInt32 propsLen = 0;
    memcpy(&propsLen, bytes.data() + off, sizeof(propsLen)); off += sizeof(propsLen);
    if (propsLen > 0 && off + propsLen <= bytes.size()) {
      std::string propsJson(reinterpret_cast<const char*>(bytes.data() + off), propsLen);
      Poco::JSON::Parser parser;
      auto res = parser.parse(propsJson);
      _properties.fromJSON(*res.extract<Poco::JSON::Object::Ptr>());
      off += propsLen;
    }
    if (off < bytes.size()) {
      setDataFromBytes(BytesVector(bytes.begin() + static_cast<ptrdiff_t>(off), bytes.end()));
    }
  } else {
    // Legacy JSON path: [8-byte propsSize][props JSON blob][data]
    // The JSON blob contains uuid/number/reliability written by old propertiesToJSON().
    if (bytes.size() < sizeof(Poco::UInt64)) return;
    Poco::UInt64 propsSize = 0;
    memcpy(&propsSize, bytes.data(), sizeof(propsSize));
    BytesVector props(propsSize);
    if (propsSize > 0 && sizeof(propsSize) + propsSize <= bytes.size()) {
      memcpy(props.data(), bytes.data() + sizeof(propsSize), propsSize);
    }
    setPropertiesFromBytes(props);
    size_t dataStart = sizeof(propsSize) + static_cast<size_t>(propsSize);
    if (dataStart < bytes.size()) {
      setDataFromBytes(BytesVector(bytes.begin() + static_cast<ptrdiff_t>(dataStart), bytes.end()));
    }
  }
}

void Message::setBoolProperty(std::string name, property::raw_type::boolean value) {
  _properties.setProperty(std::move(name), property::Bool(value));
}

void Message::setCharProperty(std::string name, property::raw_type::character value) {
  _properties.setProperty(std::move(name), property::Char(value));
}

void Message::setStringProperty(std::string name, property::raw_type::string value) {
  _properties.setProperty(std::move(name), property::String(std::move(value)));
}

void Message::setByteProperty(std::string name, property::raw_type::byte value) { _properties.setProperty(std::move(name), property::Byte(value)); }

void Message::setShortProperty(std::string name, property::raw_type::short_integer value) {
  _properties.setProperty(std::move(name), property::Short(value));
}

void Message::setIntProperty(std::string name, property::raw_type::integer value) { _properties.setProperty(std::move(name), property::Int(value)); }

void Message::setLongProperty(std::string name, property::raw_type::long_integer value) {
  _properties.setProperty(std::move(name), property::Long(value));
}

void Message::setFloatProperty(std::string name, property::raw_type::floating_point value) {
  _properties.setProperty(std::move(name), property::Float(value));
}

void Message::setDoubleProperty(std::string name, property::raw_type::double_point value) {
  _properties.setProperty(std::move(name), property::Double(value));
}

void Message::setBytesProperty(std::string name, const BytesVector& value) { _properties.setProperty(std::move(name), property::Bytes(value)); }

void Message::setPropertiesFromBytes(const BytesVector& bytes) {
  _properties.clear();
  if (!bytes.empty()) {
    Poco::JSON::Parser parser;
    std::string s(bytes.begin(), bytes.end());
    auto result = parser.parse(s);
    auto object = result.extract<Poco::JSON::Object::Ptr>();
    _number = object->getValue<int64_t>("number");
    uuid.parse(object->getValue<property::raw_type::string>("uuid"));
    reliability = (object->getValue<property::raw_type::string>("reliability") == "PERSISTENT") ? PERSISTENT : NOT_PERSISTENT;
    Poco::JSON::Object::Ptr persistInfo = object->getObject("persistentInfo");
    Poco::JSON::Object::Ptr payloadInfo = persistInfo->getObject("payload");
    persistentInfo.payload.dataPath = payloadInfo->getValue<property::raw_type::string>("dataPath");
    persistentInfo.payload.propertiesPath = payloadInfo->getValue<property::raw_type::string>("propertiesPath");
    Poco::JSON::Object::Ptr sentInfo = persistInfo->getObject("sent");
    persistentInfo.sent.dataPath = sentInfo->getValue<property::raw_type::string>("dataPath");
    persistentInfo.sent.propertiesPath = sentInfo->getValue<property::raw_type::string>("propertiesPath");
    Poco::JSON::Object::Ptr transactionInfo = persistInfo->getObject("transaction");
    persistentInfo.transaction.dataPath = transactionInfo->getValue<property::raw_type::string>("dataPath");
    persistentInfo.transaction.propertiesPath = transactionInfo->getValue<property::raw_type::string>("propertiesPath");
    Poco::JSON::Object::Ptr properties = object->getObject("properties");
    if (!object.isNull()) {
      _properties.fromJSON(*properties);
    }
  }
}
Poco::JSON::Object Message::propertiesToJSON() const {
  Poco::JSON::Object json;
  Poco::JSON::Object properties = _properties.toJSON();

  json.set("properties", properties);
  json.set("number", _number);
  json.set("uuid", uuid.toString());
  json.set("reliability", (reliability == NOT_PERSISTENT) ? "NOT_PERSISTENT" : "PERSISTENT");

  Poco::JSON::Object persistInfo;
  Poco::JSON::Object transactionInfo;
  transactionInfo.set("dataPath", persistentInfo.transaction.dataPath);
  transactionInfo.set("propertiesPath", persistentInfo.transaction.propertiesPath);
  persistInfo.set("transaction", transactionInfo);
  Poco::JSON::Object payloadInfo;
  payloadInfo.set("dataPath", persistentInfo.payload.dataPath);
  payloadInfo.set("propertiesPath", persistentInfo.payload.propertiesPath);
  persistInfo.set("payload", payloadInfo);
  Poco::JSON::Object sentInfo;
  sentInfo.set("dataPath", persistentInfo.sent.dataPath);
  sentInfo.set("propertiesPath", persistentInfo.sent.propertiesPath);
  persistInfo.set("sent", sentInfo);
  json.set("persistentInfo", persistInfo);
  return json;
}

std::string message::dump(const Message& msg) {
  Poco::JSON::Object object;
  object = msg.toJSON();
  std::stringstream ss;
  object.stringify(ss, 1);
  return ss.str();
}

BytesVector dataFromMessagePath(const Poco::Path& path) {
  BytesVector data;
  Poco::File f(path);
  if (f.isDirectory() && path.getExtension() == ".message") {
    Poco::FileInputStream fi(path.toString() + "/data");
    data.resize(f.getSize());
    fi.read((char*)&data[0], f.getSize());
    if (!fi) {
      throw Poco::ReadFileException(Poco::format("can't read message data from %s", path.toString()), -1);
    }
  }
  return data;
}

BytesVector propertiesFromMessagePath(const Poco::Path& path) {
  BytesVector data;
  Poco::File f(path);
  if (f.isDirectory() && path.getExtension() == ".message") {
    Poco::FileInputStream fi(path.toString() + "/props");
    data.resize(f.getSize());
    fi.read((char*)&data[0], f.getSize());
    if (!fi) {
      throw Poco::ReadFileException(Poco::format("can't read message properties from %s", path.toString()), -1);
    }
  }
  return data;
}

}  // namespace tiny_mq
