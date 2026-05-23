//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__MESSAGE_H_
#define TINY_MQ__MESSAGE_H_

#include <Poco/UUID.h>
#include <Poco/Any.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/Path.h>
#include <Poco/Types.h>
#include <variant>
#include <memory>
#include <map>
#include <limits>
#include <vector>
#include "ConcurrentQueueHeader.h"
#include "MessageProperty.h"

namespace tiny_mq {
class Message {
 private:
  int64_t _number{0};
  Properties _properties;
  // In-memory cache set by Consumer::preparePush so recv() can skip storage round-trips.
  // Holds [1-byte type prefix][toBytes() payload]; cleared after recv() reads it.
  std::vector<char> _cachedStorageBytes;
  // Cached storage location so acknowledgeOn/commit can remove without a UUID lookup.
  // Set to max when the record location is unknown (e.g. restart path or not-yet-committed).
  Poco::UInt32 _storageTomId{std::numeric_limits<Poco::UInt32>::max()};
  Poco::UInt64 _storageOffset{0};
  friend class Consumer;
  friend class Destination;

 protected:
  Poco::JSON::Object propertiesToJSON() const;

  Message(const Message &) = default;
  Message(Message &&) = default;
  Message &operator=(const Message &) = default;
  Message &operator=(Message &&) = default;

 public:
  using Ptr = std::shared_ptr<Message>;
  Poco::UUID uuid;
  enum Type { UNDEFINED = 0, TEXT_MESSAGE = 1, STREAM_MESSAGE = 2, BYTES_MESSAGE = 3, MAP_MESSAGE = 4, OBJECT_MESSAGE = 5 };
  enum Reliability { NOT_PERSISTENT = 0, PERSISTENT };
  Reliability reliability = NOT_PERSISTENT;

  // Standard JMS message headers (JMS 2.0 § 3.4). Persisted in the 0x02 binary
  // wire format; 0x01 records read back with default-valued Headers.
  struct Headers {
    std::string messageId;        // JMSMessageID, typically "ID:<uuid>"
    int64_t     timestamp     = 0;  // JMSTimestamp, ms since epoch
    int64_t     expiration    = 0;  // JMSExpiration, 0 = never
    int64_t     deliveryTime  = 0;  // JMSDeliveryTime (JMS 2.0), 0 = immediate
    int32_t     priority      = 4;  // JMSPriority, 0..9, default 4
    int32_t     deliveryCount = 0;  // JMSXDeliveryCount
    bool        redelivered   = false;
    std::string replyTo;          // destination URI
    std::string correlationId;
    std::string type;             // JMSType
  };
  Headers jmsHeaders;
  struct PersistentInfo {
    struct Transaction {
      std::string dataPath;
      std::string propertiesPath;
    };
    Transaction transaction;
    struct Payload {
      std::string dataPath;
      std::string propertiesPath;
    };
    Payload payload;
    struct Sent {
      std::string dataPath;
      std::string propertiesPath;
    };
    Sent sent;
  };

  Message() = default;
  virtual ~Message() = default;

  PersistentInfo persistentInfo;
  bool isPersistent() const;

  int64_t number() const;

  template <typename MessageType>
  static typename MessageType::Ptr As(const Message::Ptr &pmessage) {
    return std::dynamic_pointer_cast<MessageType>(pmessage);
  };

  template <typename T, Properties::TypeIsProperty<T> = 0>
  void setProperty(std::string name, T value) {
    _properties.setProperty(std::move(name), std::move(value));
  }
  void setBoolProperty(std::string name, property::raw_type::boolean value);
  void setCharProperty(std::string name, property::raw_type::character value);
  void setStringProperty(std::string name, property::raw_type::string value);
  void setByteProperty(std::string name, property::raw_type::byte value);
  void setShortProperty(std::string name, property::raw_type::short_integer value);
  void setIntProperty(std::string name, property::raw_type::integer value);
  void setLongProperty(std::string name, property::raw_type::long_integer value);
  void setFloatProperty(std::string name, property::raw_type::floating_point value);
  void setDoubleProperty(std::string name, property::raw_type::double_point value);
  void setBytesProperty(std::string name, const BytesVector &value);
  template <typename T, Properties::TypeIsProperty<T> = 0>
  const T &property(const std::string &name) const {
    return _properties.template property<T>(name);
  }
  bool hasProperty(const std::string &name) const;
  property::ValueType propertyValueType(const std::string &name) const;
  virtual BytesVector dataAsBytes() const = 0;
  BytesVector propertiesAsBytes() const;
  BytesVector toBytes() const;
  virtual void setDataFromBytes(const BytesVector &bytes) = 0;
  void setPropertiesFromBytes(const BytesVector &bytes);
  void fromBytes(const BytesVector &bytes);
  virtual void clearData() = 0;
  virtual Poco::JSON::Object toJSON() const = 0;
  virtual Message::Ptr copy() const = 0;
  virtual Type type() const = 0;
};
namespace message {
std::string dump(const Message &msg);
}
BytesVector dataFromMessagePath(const Poco::Path &path);
BytesVector propertiesFromMessagePath(const Poco::Path &path);
}  // namespace tiny_mq
using QueueT = moodycamel::BlockingConcurrentQueue<tiny_mq::Message::Ptr>;

#endif  // TINY_MQ__MESSAGE_H_
