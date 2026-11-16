//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__PRODUCER_H_
#define TINY_MQ__PRODUCER_H_

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include "Message.h"

namespace tiny_mq {
class Destination;
class Session;
class SessionDispatch;

// Per-send delivery options (JMS 2.0 § 7.6). Override the deliveryMode/priority/
// TTL/delay applied to a message at send time. Used both as the per-send
// argument and as the producer's stored defaults (see Producer::setDefault).
struct SendOptions {
  Message::Reliability deliveryMode = Message::NOT_PERSISTENT;
  int32_t priority = 4;          // JMSPriority, 0..9 (4 = normal)
  int64_t timeToLive = 0;        // ms; 0 = never expire
  int64_t deliveryDelay = 0;     // ms; 0 = immediate at publication
};

// An accepted send reports either its provider headers or its operation error.
struct SendCompletion {
  std::optional<Message::Headers> headers;
  std::exception_ptr error;
};

class Producer {
  Poco::UUID _uuid;
  std::shared_ptr<Destination> _destination;
  std::atomic<Session*> _session;
  std::atomic_bool _closed{false};
  bool _staged = false;
  bool _disableMessageID{false};
  bool _disableMessageTimestamp{false};
  // Set only via setDefault(). When unset, plain send() preserves the message's
  // own reliability/priority (backward-compatible with create-time settings).
  std::optional<SendOptions> _default;

 public:
  using Ptr = std::shared_ptr<Producer>;
  using CompletionListener = std::function<void(const SendCompletion&)>;
  void sendAsync(const Message& message, CompletionListener listener);
  void sendAsync(const Message& message, const SendOptions& options, CompletionListener listener);
  void close();
  // Uses producer defaults (set via setDefault); otherwise the message's own
  // reliability/priority are left untouched.
  void send(const Message &message);
  // Per-send override: opts replaces the message's deliveryMode/priority/TTL/delay.
  void send(const Message &message, const SendOptions &opts);
  // Stores defaults applied by the no-argument send(). Validates priority/TTL.
  void setDefault(const SendOptions &opts);
  const std::string &transactionId() const;
  virtual ~Producer();
  const Poco::UUID &id() const;

  // JMS 2.0 § 7.5 — hints to skip provider-assigned MessageID / Timestamp.
  void setDisableMessageID(bool value);
  bool isDisableMessageID() const;
  void setDisableMessageTimestamp(bool value);
  bool isDisableMessageTimestamp() const;

 private:
  Producer(Destination& destination, Session& session);
  Session& ensureOpen() const;
  void accept(const Message& message, const SendOptions* options, CompletionListener listener);
  void dispatch(Session& session, const Message& message, const SendOptions* options,
                SendCompletion* completion = nullptr);

  friend class Destination;
  friend class Consumer;
  friend class Session;
  friend class SessionDispatch;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__PRODUCER_H_
