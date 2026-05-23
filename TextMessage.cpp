//
// Created by Alexander Bychuk on 22.02.2022.
//

#include "TextMessage.h"

namespace tiny_mq {
TextMessage::TextMessage(Poco::UUID uuid_, Reliability reliability_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
TextMessage::TextMessage(Poco::UUID uuid_, std::string text_, Reliability reliability_) : _text(std::move(text_)) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
const std::string& TextMessage::text() const { return _text; }
void TextMessage::setText(const std::string& text) { _text = text; }
void TextMessage::clearData() { _text.clear(); }
Poco::JSON::Object TextMessage::toJSON() const {
  auto json = propertiesToJSON();
  json.set("data", text());
  return json;
}
Message::Ptr TextMessage::copy() const { return tiny_mq::Message::Ptr(new TextMessage(*this)); }

Message::Type TextMessage::type() const { return Message::TEXT_MESSAGE; }

BytesVector TextMessage::dataAsBytes() const { return BytesVector{_text.begin(), _text.cend()}; }

void TextMessage::setDataFromBytes(const BytesVector& bytes) { _text = std::string((char*)&bytes[0], bytes.size()); }

}  // namespace tiny_mq
