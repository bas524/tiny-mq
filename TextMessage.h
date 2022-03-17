//
// Created by Alexander Bychuk on 22.02.2022.
//

#ifndef TINY_MQ__TEXTMESSAGE_H_
#define TINY_MQ__TEXTMESSAGE_H_

#include "Message.h"

namespace tiny_mq {
class TextMessage : public Message {
 public:
  using Ptr = std::shared_ptr<TextMessage>;
  TextMessage() = default;
  explicit TextMessage(const std::string &text);
  ~TextMessage() override = default;
  const std::string &text() const;
  void setText(const std::string &text);
  void clearData() override;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__TEXTMESSAGE_H_
