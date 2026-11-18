//
// Created by Alexander Bychuk on 22.02.2022.
//

#include "TextMessage.h"

namespace tiny_mq {
namespace {
constexpr size_t minReusableText = 64;
constexpr size_t maxReusableText = 4096;
constexpr size_t maxReusableCopyBlock = 1024;

struct TextCopyBuffer;
struct TextCopyBufferSlot {
  TextCopyBuffer* active = nullptr;
  bool retired = false;
};
constinit thread_local TextCopyBufferSlot copyBufferSlot;

// Bounded storage per thread that copies long text. Destruction never
// initializes this cache and can still run after the cache has been torn down.
struct TextCopyBuffer {
  std::string text;
  std::unique_ptr<void, void (*)(void*)> copyBlock{nullptr, nullptr};
  TextCopyBuffer() { copyBufferSlot.active = this; }
  ~TextCopyBuffer() { copyBufferSlot = {nullptr, true}; }
};

template<class T>
struct TextCopyAllocator {
  using value_type = T;
  using is_always_equal = std::true_type;

  TextCopyAllocator() = default;
  template<class U>
  TextCopyAllocator(const TextCopyAllocator<U>&) noexcept {}
  template<class U>
  bool operator==(const TextCopyAllocator<U>&) const noexcept { return true; }

  T* allocate(size_t count) {
    if (auto* buffer = copyBufferSlot.active; buffer && count == 1 && sizeof(T) <= maxReusableCopyBlock) {
      auto& block = buffer->copyBlock;
      if (block && block.get_deleter() == &releaseBlock) return static_cast<T*>(block.release());
    }
    return std::allocator<T>{}.allocate(count);
  }

  void deallocate(T* pointer, size_t count) noexcept {
    // The control block reaches the allocator only after its last weak owner.
    if (auto* buffer = copyBufferSlot.active; buffer && count == 1 && sizeof(T) <= maxReusableCopyBlock &&
        !buffer->copyBlock) {
      buffer->copyBlock = {pointer, &releaseBlock};
      return;
    }
    std::allocator<T>{}.deallocate(pointer, count);
  }

 private:
  static void releaseBlock(void* pointer) noexcept { std::allocator<T>{}.deallocate(static_cast<T*>(pointer), 1); }
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
Message::Ptr TextMessage::copy() const {
  if (_text.size() >= minReusableText && _text.size() <= maxReusableText) {
    return std::allocate_shared<TextMessage>(TextCopyAllocator<TextMessage>{}, *this);
  }
  return std::make_shared<TextMessage>(*this);
}

Message::Type TextMessage::type() const { return Message::TEXT_MESSAGE; }

BytesVector TextMessage::dataAsBytes() const { return BytesVector{_text.begin(), _text.cend()}; }

void TextMessage::setDataFromBytes(const BytesVector& bytes) {
  _text.assign(bytes.begin(), bytes.end());
  jmsHeaders.nullText = false;
}

}  // namespace tiny_mq
