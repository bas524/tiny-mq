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
  ~MapMessage() override = default;
  template <typename T, Properties::TypeIsProperty<T> = 0>
  void set(const std::string &name, const T &value) {
    _bodyProps.template setProperty(name, value);
  }
  template <typename T>
  std::enable_if_t<property::IsValidProperty<T>, const T &> get(const std::string &name) const {
    return _bodyProps.template getProperty<T>(name);
  }
  bool has(const std::string &name) const;
  bool empty() const;
  std::vector<std::string> names() const;
  void clearData() override;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__MAPMESSAGE_H_
