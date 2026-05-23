//
// Created by Alexander Bychuk on 22.02.2022.
//

#ifndef TINY_MQ__TEXTMESSAGE_H_
#define TINY_MQ__TEXTMESSAGE_H_

#include "Message.h"

namespace tiny_mq {
class TextMessage : public Message {
  std::string _text;

 public:
  using Ptr = std::shared_ptr<TextMessage>;
  TextMessage() = default;
  TextMessage(Poco::UUID uuid_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  explicit TextMessage(Poco::UUID uuid_, std::string text_, Message::Reliability reliability_ = Message::NOT_PERSISTENT);
  ~TextMessage() override = default;

  TextMessage(const TextMessage &) = default;
  TextMessage(TextMessage &&) = default;
  TextMessage &operator=(const TextMessage &) = default;
  TextMessage &operator=(TextMessage &&) = default;

  const std::string &text() const;
  void setText(const std::string &text);
  void clearData() override;
  Poco::JSON::Object toJSON() const override;
  Message::Ptr copy() const override;
  Message::Type type() const override;

  BytesVector dataAsBytes() const override;
  void setDataFromBytes(const BytesVector &bytes) override;
};
}  // namespace tiny_mq

#endif  // TINY_MQ__TEXTMESSAGE_H_
