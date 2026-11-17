//
// Created by Alexander Bychuk on 22.02.2022.
//

#include "TextMessage.h"

namespace tiny_mq {
namespace {
constexpr size_t minReusableText = 64;
constexpr size_t maxReusableText = 4096;

struct TextCopyBuffer;
struct TextCopyBufferSlot {
  TextCopyBuffer* active = nullptr;
  bool retired = false;
};
constinit thread_local TextCopyBufferSlot copyBufferSlot;

// One bounded buffer per thread that copies long text. Destruction never
// initializes this cache and can still run after the cache has been torn down.
struct TextCopyBuffer {
  std::string text;
  TextCopyBuffer() { copyBufferSlot.active = this; }
  ~TextCopyBuffer() { copyBufferSlot = {nullptr, true}; }
};

std::string copyText(const std::string& text) {
  if (text.size() < minReusableText || text.size() > maxReusableText) return text;
  auto* buffer = copyBufferSlot.active;
  if (!buffer) {
    // Later TLS destructors may still copy a message on this thread.
    if (copyBufferSlot.retired) return text;
    thread_local TextCopyBuffer owned;
    buffer = &owned;
  }
  if (buffer->text.capacity() < text.size()) return text;
  std::string copy = std::move(buffer->text);
  copy.assign(text);
  return copy;
}
}  // namespace

TextMessage::TextMessage(Poco::UUID uuid_, Reliability reliability_) {
  uuid = std::move(uuid_);
  reliability = reliability_;
  jmsHeaders.nullText = true;
}
TextMessage::TextMessage(Poco::UUID uuid_, std::string text_, Reliability reliability_) : _text(std::move(text_)) {
  uuid = std::move(uuid_);
  reliability = reliability_;
}
TextMessage::TextMessage(const TextMessage& other) : Message(other), _text(copyText(other._text)) {}

TextMessage::~TextMessage() {
  const auto capacity = _text.capacity();
  if (capacity < minReusableText || capacity > maxReusableText) return;
  if (auto* buffer = copyBufferSlot.active; buffer && buffer->text.capacity() < capacity) {
    buffer->text = std::move(_text);
  }
}

const std::string& TextMessage::text() const { return _text; }
void TextMessage::setText(const std::string& text) {
  _text = text;
  jmsHeaders.nullText = false;
}
void TextMessage::clearData() {
  _text.clear();
  jmsHeaders.nullText = true;
}
Poco::JSON::Object TextMessage::toJSON() const {
  auto json = propertiesToJSON();
  json.set("data", isTextNull() ? Poco::Dynamic::Var{} : Poco::Dynamic::Var(text()));
  return json;
}
Message::Ptr TextMessage::copy() const { return std::make_shared<TextMessage>(*this); }

Message::Type TextMessage::type() const { return Message::TEXT_MESSAGE; }

BytesVector TextMessage::dataAsBytes() const { return BytesVector{_text.begin(), _text.cend()}; }

void TextMessage::setDataFromBytes(const BytesVector& bytes) {
  _text.assign(bytes.begin(), bytes.end());
  jmsHeaders.nullText = false;
}

}  // namespace tiny_mq
