//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__PRODUCER_H_
#define TINY_MQ__PRODUCER_H_

#include <memory>
#include <optional>
#include <Poco/Logger.h>
#include "Message.h"
#include "BlockingConcurrentQueueHeader.h"

namespace tiny_mq {
class Destination;
class Session;

// Per-send delivery options (JMS 2.0 § 7.6). Override the deliveryMode/priority/
// TTL/delay applied to a message at send time. Used both as the per-send
// argument and as the producer's stored defaults (see Producer::setDefault).
struct SendOptions {
  Message::Reliability deliveryMode = Message::NOT_PERSISTENT;
  int32_t priority = 4;          // JMSPriority, 0..9 (4 = normal)
  int64_t timeToLive = 0;        // ms; 0 = never expire
  int64_t deliveryDelay = 0;     // ms; 0 = immediate (consumed by spec 13)
};

class Producer {
  Poco::UUID _uuid;
  std::reference_wrapper<Destination> _destination;
  std::unique_ptr<QueueT::producer_token_t> _token;
  std::reference_wrapper<Poco::Logger> _logger;
  std::reference_wrapper<Session> _session;
  std::shared_ptr<QueueT> _transactQueue;
  bool _disableMessageID{false};
  bool _disableMessageTimestamp{false};
  // Set only via setDefault(). When unset, plain send() preserves the message's
  // own reliability/priority (backward-compatible with create-time settings).
  std::optional<SendOptions> _default;

 public:
  using Ptr = std::shared_ptr<Producer>;
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
  explicit Producer(Destination &destination,
                    Session &session,
                    const Poco::UUID &uuid,
                    std::unique_ptr<QueueT::producer_token_t> token);
  const QueueT::producer_token_t &token() const;
  // Apply per-send options to the message (deliveryMode/priority/expiration/deliveryTime).
  static void applyOptions(Message &message, const SendOptions &opts);
  // Fill provider headers (messageId/timestamp) and hand off to the destination.
  void dispatch(const Message &message);
  void commit(const std::string& transactionId);
  void rollback(const std::string& transactionId);
  QueueT &transactQueue() const;
  Destination &destination() const;

  friend class Destination;
  friend class Consumer;
  friend class Session;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__PRODUCER_H_
