//
// ObjectMessage — JMS 2.0 § 3.11.4. Broker is payload-opaque: the body is an
// already-serialized blob plus a className hint; (de)serialization of the object
// is the client's responsibility.
//

#ifndef TINY_MQ__OBJECTMESSAGE_H_
#define TINY_MQ__OBJECTMESSAGE_H_

#include "Message.h"

namespace tiny_mq {
class ObjectMessage : public Message {
  BytesVector _body;
  std::string _className;

 public:
  using Ptr = std::shared_ptr<ObjectMessage>;
  ObjectMessage() = default;
  explicit ObjectMessage(Poco::UUID uuid_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  explicit ObjectMessage(Poco::UUID uuid_, BytesVector body_, std::string className_,
                         Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  ~ObjectMessage() override = default;

  ObjectMessage(const ObjectMessage &) = default;
  ObjectMessage(ObjectMessage &&) = default;
  ObjectMessage &operator=(const ObjectMessage &) = default;
  ObjectMessage &operator=(ObjectMessage &&) = default;

  void setBody(BytesVector serialized, std::string className);
  const BytesVector &body() const;
  const std::string &className() const;

  void clearData() override;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
  Message::Type type() const override;

  BytesVector dataAsBytes() const override;
  void setDataFromBytes(const BytesVector &bytes) override;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__OBJECTMESSAGE_H_
