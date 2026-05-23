//
// Created by Alexander Bychuk on 26.03.2022.
//

#ifndef TINY_MQ__MAPMESSAGE_H_
#define TINY_MQ__MAPMESSAGE_H_

#include "Message.h"

namespace tiny_mq {
class MapMessage : public Message {
  Properties _bodyProps;

 public:
  using Ptr = std::shared_ptr<MapMessage>;
  MapMessage() = default;
  MapMessage(Poco::UUID uuid_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  ~MapMessage() override = default;

  MapMessage(const MapMessage &) = default;
  MapMessage(MapMessage &&) = default;
  MapMessage &operator=(const MapMessage &) = default;
  MapMessage &operator=(MapMessage &&) = default;

  template <typename T, Properties::TypeIsProperty<T> = 0>
  void set(std::string name, T value) {
    _bodyProps.setProperty(std::move(name), std::move(value));
  }
  template <typename T>
  std::enable_if_t<property::IsValidProperty<T>, const T &> get(const std::string &name) const {
    return _bodyProps.template property<T>(name);
  }
  bool has(const std::string &name) const;
  bool empty() const;
  std::vector<std::string> names() const;
  property::ValueType valueType(const std::string &name) const;
  void clearData() override;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
  Message::Type type() const override;

  BytesVector dataAsBytes() const override;
  void setDataFromBytes(const BytesVector &bytes) override;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__MAPMESSAGE_H_
