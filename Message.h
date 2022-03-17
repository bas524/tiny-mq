//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__MESSAGE_H_
#define TINY_MQ__MESSAGE_H_

#include <Poco/UUID.h>
#include <Poco/Any.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <variant>
#include <memory>
#include <map>
#include "ConcurrentQueueHeader.h"
#include "MessageProperty.h"
namespace tiny_mq {
class Message {
 private:
  int64_t _number{0};
  Properties _properties;
  friend class Consumer;

 protected:
  Poco::Any data;
  Poco::JSON::Object propertytoJSON() const;

 public:
  using Ptr = std::shared_ptr<Message>;
  Poco::UUID uuid;
  enum Reliability { NOT_PERSISTENT = 0, PERSISTENT };
  Reliability reliability;
  struct PersistentInfo {
    std::string fileFromName;
    std::string fileToName;
  };
  virtual ~Message() = default;

  PersistentInfo persistentInfo;
  bool isPersistent() const;

  template <typename MessageType>
  static typename MessageType::Ptr As(const Message::Ptr &pmessage) {
    return std::dynamic_pointer_cast<MessageType>(pmessage);
  };

  template <typename T, Properties::TypeIsProperty<T> = 0>
  void setProperty(const std::string &name, const Poco::Nullable<T> &value) {
    _properties.template setProperty(name, value);
  }
  template <typename T, Properties::TypeIsProperty<T> = 0>
  const Poco::Nullable<T> &getProperty(const std::string &name) const {
    return _properties.template getProperty<T>(name);
  }
  bool hasProperty(const std::string &name) const;
  virtual void clearData();
  virtual Poco::JSON::Object toJSON() const = 0;
  virtual Message::Ptr copy() const = 0;
};
}  // namespace tiny_mq
using QueueT = moodycamel::BlockingConcurrentQueue<tiny_mq::Message::Ptr>;

#endif  // TINY_MQ__MESSAGE_H_
