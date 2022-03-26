//
// Created by Alexander Bychuk on 22.02.2022.
//

#include "TextMessage.h"

namespace tiny_mq {
TextMessage::TextMessage(const std::string& text) { data = text; }
const std::string& TextMessage::text() const { return Poco::RefAnyCast<std::string>(data); }
void TextMessage::setText(const std::string& text) { data = text; }
void TextMessage::clearData() { data = std::string(); }
Poco::JSON::Object TextMessage::toJSON() const {
  auto json = propertiesToJSON();
  json.set("data", text());
  return json;
}
Message::Ptr TextMessage::copy() const { return tiny_mq::Message::Ptr(new TextMessage(*this)); }
}  // namespace tiny_mq