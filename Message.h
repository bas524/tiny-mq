//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__MESSAGE_H_
#define TINY_MQ__MESSAGE_H_

#include <Poco/UUID.h>
#include <Poco/Any.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/Path.h>
#include <Poco/Types.h>
#include <variant>
#include <memory>
#include <map>
#include <limits>
#include <vector>
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include "ConcurrentQueueHeader.h"
#include "MessageProperty.h"

namespace tiny_mq {

// Shared upper bound for "how far in the future" a JMS header may point:
// SendOptions::deliveryDelay / timeToLive (validated in Producer's
// validateSendOptions, the ingress) and jmsHeaders.deliveryTime (re-checked at
// every point it can enter the system besides that ingress — see spec 13
// review round 2, B4). One constant, not two, on purpose: both quantities are
// "ms from now" bounds guarding the exact same arithmetic hazard —
// now + delay overflowing int64_t, and the ms -> native-duration conversion
// inside DeliveryScheduler's wait_until overflowing for deliveryTime — so a
// single shared ceiling is the honest description of the invariant, not a
// coincidence to be split apart. 10 years in ms; far beyond any legitimate
// delay/TTL, comfortably below where the conversions overflow.
constexpr int64_t kMaxFutureHeaderMs = 315360000000LL;  // 10 years

class Message {
 private:
  int64_t _number{0};
  Properties _properties;
  // In-memory cache set by Consumer::preparePush so recv() can skip storage round-trips.
  // Holds [1-byte type prefix][toBytes() payload]; cleared after recv() reads it.
  std::vector<char> _cachedStorageBytes;
  // Cached storage location so acknowledgeOn/commit can remove without a UUID lookup.
  // Set to max when the record location is unknown (e.g. restart path or not-yet-committed).
  Poco::UInt32 _storageTomId{std::numeric_limits<Poco::UInt32>::max()};
  Poco::UInt64 _storageOffset{0};
  // Transient (never serialized): for a transactional send, the delay in ms
  // requested via SendOptions::deliveryDelay. Resolved to an absolute
  // jmsHeaders.deliveryTime at commit time (spec 13 — the clock starts at
  // commit, not send), by Producer::commit.
  int64_t _pendingDeliveryDelay{0};
  friend class Consumer;
  friend class Destination;
  friend class Producer;

 protected:
  Poco::JSON::Object propertiesToJSON() const;

  Message(const Message &) = default;
  Message(Message &&) = default;
  Message &operator=(const Message &) = default;
  Message &operator=(Message &&) = default;

 public:
  using Ptr = std::shared_ptr<Message>;
  Poco::UUID uuid;
  enum Type { UNDEFINED = 0, TEXT_MESSAGE = 1, STREAM_MESSAGE = 2, BYTES_MESSAGE = 3, MAP_MESSAGE = 4, OBJECT_MESSAGE = 5 };
  enum Reliability { NOT_PERSISTENT = 0, PERSISTENT };
  Reliability reliability = NOT_PERSISTENT;

  // Standard JMS message headers (JMS 2.0 § 3.4). Persisted in the 0x02 binary
  // wire format; 0x01 records read back with default-valued Headers.
  struct Headers {
    std::string messageId;        // JMSMessageID, typically "ID:<uuid>"
    int64_t     timestamp     = 0;  // JMSTimestamp, ms since epoch
    int64_t     expiration    = 0;  // JMSExpiration, 0 = never
    int64_t     deliveryTime  = 0;  // JMSDeliveryTime (JMS 2.0), 0 = immediate
    int32_t     priority      = 4;  // JMSPriority, 0..9, default 4
    int32_t     deliveryCount = 0;  // JMSXDeliveryCount
    bool        redelivered   = false;
    std::string replyTo;          // destination URI
    std::string correlationId;
    std::string type;             // JMSType
  };
  Headers jmsHeaders;
  struct PersistentInfo {
    struct Transaction {
      std::string dataPath;
      std::string propertiesPath;
    };
    Transaction transaction;
    struct Payload {
      std::string dataPath;
      std::string propertiesPath;
    };
    Payload payload;
    struct Sent {
      std::string dataPath;
      std::string propertiesPath;
    };
    Sent sent;
  };

  Message() = default;
  virtual ~Message() = default;

  PersistentInfo persistentInfo;
  bool isPersistent() const;
  // JMSExpiration (spec 44): true when a non-zero expiration lies at or behind nowMs.
  bool isExpired(int64_t nowMs) const;

  int64_t number() const;

  // Patch the deliveryTime field of an already-serialized _cachedStorageBytes
  // cache in place (no re-serialization). Used by Producer::commit to resolve
  // a transactional send's delay clock (JMS 2.0 §7.8, spec 13): the clock
  // starts at commit, not send, but Consumer::preparePush already cached the
  // pre-commit bytes (with deliveryTime == 0) at send time. Without this, the
  // fast recv() path (Consumer::recv, which reads the cache instead of
  // storage) would hand the application a message whose JMSDeliveryTime is 0
  // even though it really was delayed and delivered on schedule. No-op if
  // there is no cache, or the cache predates the 0x02 wire format (see
  // toBytes()/fromBytes() for the offset table this mirrors).
  void patchCachedDeliveryTime(int64_t deliveryTime);

  template <typename MessageType>
  static typename MessageType::Ptr As(const Message::Ptr &pmessage) {
    return std::dynamic_pointer_cast<MessageType>(pmessage);
  };

  template <typename T, Properties::TypeIsProperty<T> = 0>
  void setProperty(std::string name, T value) {
    _properties.setProperty(std::move(name), std::move(value));
  }
  void setBoolProperty(std::string name, property::raw_type::boolean value);
  void setCharProperty(std::string name, property::raw_type::character value);
  void setStringProperty(std::string name, property::raw_type::string value);
  void setByteProperty(std::string name, property::raw_type::byte value);
  void setShortProperty(std::string name, property::raw_type::short_integer value);
  void setIntProperty(std::string name, property::raw_type::integer value);
  void setLongProperty(std::string name, property::raw_type::long_integer value);
  void setFloatProperty(std::string name, property::raw_type::floating_point value);
  void setDoubleProperty(std::string name, property::raw_type::double_point value);
  void setBytesProperty(std::string name, const BytesVector &value);
  template <typename T, Properties::TypeIsProperty<T> = 0>
  const T &property(const std::string &name) const {
    return _properties.template property<T>(name);
  }
  bool hasProperty(const std::string &name) const;
  property::ValueType propertyValueType(const std::string &name) const;
  virtual BytesVector dataAsBytes() const = 0;
  BytesVector propertiesAsBytes() const;
  BytesVector toBytes() const;
  virtual void setDataFromBytes(const BytesVector &bytes) = 0;
  void setPropertiesFromBytes(const BytesVector &bytes);
  void fromBytes(const BytesVector &bytes);
  virtual void clearData() = 0;
  virtual Poco::JSON::Object toJSON() const = 0;
  virtual Message::Ptr copy() const = 0;
  virtual Type type() const = 0;
};
namespace message {
std::string dump(const Message &msg);
}
BytesVector dataFromMessagePath(const Poco::Path &path);
BytesVector propertiesFromMessagePath(const Poco::Path &path);
}  // namespace tiny_mq

// ---------------------------------------------------------------------------
// PriorityQueueT — 10-band JMSPriority-aware queue (spec 45).
//
// Partitions messages into 10 priority bands (0..9).  A consumer polls bands
// 9→0, taking the first non-empty band, and drains FIFO within each band.
// For uniform priority (default p=4) the ordering is identical to FIFO; the
// extra overhead is a bounded scan of at most 5 empty bands (9 down to 5).
//
// API surface matches the subset of moodycamel::BlockingConcurrentQueue used
// by Consumer and Producer; consumer/producer tokens are kept as lightweight
// no-ops so call sites need no changes.
// ---------------------------------------------------------------------------
class PriorityQueueT {
 public:
  static constexpr int32_t kBands = 10;

  // Lightweight no-op token types — preserved for API compatibility.
  struct producer_token_t {
    explicit producer_token_t(PriorityQueueT&) noexcept {}
    producer_token_t(const producer_token_t&) = default;
    producer_token_t& operator=(const producer_token_t&) = default;
  };
  struct consumer_token_t {
    explicit consumer_token_t(PriorityQueueT&) noexcept {}
    consumer_token_t(const consumer_token_t&) = default;
    consumer_token_t& operator=(const consumer_token_t&) = default;
  };

  PriorityQueueT() = default;
  PriorityQueueT(const PriorityQueueT&) = delete;
  PriorityQueueT& operator=(const PriorityQueueT&) = delete;

  // Enqueue: routes to the priority band matching message->jmsHeaders.priority.
  void enqueue(tiny_mq::Message::Ptr msg) {
    int32_t prio = msg ? std::clamp(msg->jmsHeaders.priority, int32_t{0}, int32_t{9}) : int32_t{4};
    _bands[static_cast<size_t>(prio)].enqueue(std::move(msg));
    // Bit is set, and the semaphore signaled, only after the band enqueue is
    // visible, so neither the mask nor a woken waiter can observe "empty" when
    // a message is actually present (see dequeueFromBands).
    _nonEmpty.fetch_or(static_cast<uint16_t>(1u << prio), std::memory_order_release);
    _sema.signal();  // counting semaphore: wakes a blocked consumer, one token per message
  }

  // Tokened enqueue — token is a no-op; priority is taken from the message.
  void enqueue(producer_token_t const& /*tok*/, tiny_mq::Message::Ptr msg) {
    enqueue(std::move(msg));
  }

  // Blocking dequeue with microsecond timeout (consumer-token overload).
  // Waits on the semaphore (one token per enqueued message), then takes the
  // highest-priority non-empty band.
  bool wait_dequeue_timed(consumer_token_t& /*tok*/, tiny_mq::Message::Ptr& msg, int64_t usec_timeout) {
    if (!_sema.wait(usec_timeout)) return false;
    return dequeueFromBandsOrReturnToken(msg);
  }

  // Tokenless blocking dequeue — used by Consumer::rollback via _transactQueue.
  bool wait_dequeue_timed(tiny_mq::Message::Ptr& msg, int64_t usec_timeout) {
    if (!_sema.wait(usec_timeout)) return false;
    return dequeueFromBandsOrReturnToken(msg);
  }

  // Non-blocking dequeue.
  bool try_dequeue(tiny_mq::Message::Ptr& msg) {
    if (!_sema.tryWait()) return false;
    return dequeueFromBandsOrReturnToken(msg);
  }

 private:
  // Takes the message from the highest-priority non-empty band, using the
  // _nonEmpty bitmask to skip empty bands (spec 45 fast path). The mask may
  // lag towards "band looks non-empty but is actually drained" (benign race
  // with a concurrent dequeuer) — that's handled by clearing the bit and
  // continuing the walk. It must never lag the other way (mask says empty
  // while a message is present): a bit is only set in enqueue() after the
  // band's enqueue has already happened, so any thread observing the signal
  // for that message is guaranteed (by the queue's own synchronization) to
  // also observe the bit. If the mask-guided walk still comes up empty despite
  // a signal having fired, a full 9->0 scan runs as a safety net.
  bool dequeueFromBands(tiny_mq::Message::Ptr& msg) {
    uint16_t mask = _nonEmpty.load(std::memory_order_acquire);
    while (mask != 0) {
      auto band = static_cast<int32_t>((sizeof(uint16_t) * 8 - 1) - std::countl_zero(mask));
      if (_bands[static_cast<size_t>(band)].try_dequeue(msg)) return true;
      _nonEmpty.fetch_and(static_cast<uint16_t>(~(1u << band)), std::memory_order_relaxed);
      mask = _nonEmpty.load(std::memory_order_acquire);
    }
    for (int32_t band = kBands - 1; band >= 0; --band) {
      if (_bands[static_cast<size_t>(band)].try_dequeue(msg)) return true;
    }
    return false;
  }

  // Called after a successful semaphore acquire. Because every enqueue signals
  // exactly once and every message is consumed by exactly one successful
  // dequeueFromBands(), the token this call holds is guaranteed to correspond
  // to *some* present message — but with several concurrent consumers it may
  // not be the specific one this thread expects (e.g. another consumer's
  // dequeueFromBands() already grabbed it via the mask/full-scan walk before
  // this thread ran). If our own scan still finds nothing, the token is
  // spurious for us: hand it back with signal() so whichever consumer holds
  // the real message's token eventually wakes, rather than silently dropping
  // a wakeup for a message that is genuinely enqueued.
  bool dequeueFromBandsOrReturnToken(tiny_mq::Message::Ptr& msg) {
    if (dequeueFromBands(msg)) return true;
    _sema.signal();
    return false;
  }

  std::array<moodycamel::ConcurrentQueue<tiny_mq::Message::Ptr>, kBands> _bands;
  moodycamel::LightweightSemaphore _sema{0};
  std::atomic<uint16_t> _nonEmpty{0};
};

using QueueT = PriorityQueueT;

#endif  // TINY_MQ__MESSAGE_H_
