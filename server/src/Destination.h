#ifndef TINY_MQ__DESTINATION_H_
#define TINY_MQ__DESTINATION_H_

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <list>
#include <map>
#include <vector>
#include <Poco/Path.h>
#include "Consumer.h"
#include "DeliveryScheduler.h"
#include "DestinationType.h"
#include "Exceptions.h"
#include "Producer.h"
#include "RedeliveryPolicy.h"
#include "SubscriptionRegistry.h"

namespace tiny_mq {
class Connection;
class Exchange;
class QueueBrowser;
class Session;
class TemporaryDestination;
class LocalTransactionJournal;

// Cheap uncontended locking for the ready set and the publication target list.
class DestinationMutex {
 public:
  bool try_lock() noexcept {
    uint8_t unlocked = 0;
    return _state.compare_exchange_strong(unlocked, 1, std::memory_order_acquire, std::memory_order_relaxed);
  }

  void lock() noexcept {
    if (try_lock()) return;
    while (_state.exchange(2, std::memory_order_acquire) != 0) _state.wait(2, std::memory_order_relaxed);
  }

  void unlock() noexcept {
    if (_state.exchange(0, std::memory_order_release) == 2) _state.notify_one();
  }

 private:
  std::atomic<uint8_t> _state{0};  // unlocked, locked, locked with contention
};

// The ready set belongs to an individual queue or topic subscription.
class DeliveryState {
 public:
  void enqueue(Message::Ptr message);
  Message::Ptr claim(const Selector* selector, int64_t timeout, std::chrono::steady_clock::time_point& deadline,
                     const std::atomic_bool& started, const std::atomic_bool& closing,
                     const std::function<bool(const Message&)>* check, bool& incompatible);
  std::vector<Message::Ptr> snapshot(const Selector* selector);
  void close();
  bool isClosed() const { return _closed.load(std::memory_order_acquire); }

  // Only used by durable subscriptions. Consumers hold these claims for their
  // session even after they detach from the subscription.
  std::atomic_size_t pending{0};
  std::mutex storageMutex;
  bool retired{false};  // guarded by storageMutex

 private:
  struct Waiter {
    explicit Waiter(const Selector* filter) : selector(filter) {}
    const Selector* selector;
    std::condition_variable_any condition;
    bool signalled{false};
  };

  void insert(Message::Ptr message, bool special);
  Message::Ptr take(const Selector* selector, const std::function<bool(const Message&)>* check,
                    bool& incompatible);
  bool followsRecoveredDelivery(const Message& message, const Selector* selector) const;
  template<class Range>
  void enqueueBatch(Range& entries) {
    std::lock_guard lock(_mutex);
    if (_closed) throw IllegalStateException("transaction destination is closed");
    const auto previousMask = _nonEmpty;
    auto next = entries.begin();
    try {
      for (; next != entries.end(); ++next) {
        const auto band = next->message->jmsHeaders.priority;
        _bands[band].push_back(std::move(next->message));
        _nonEmpty |= 1u << band;
      }
    } catch (...) {
      while (next != entries.begin()) {
        // Keep the original drafts and ready set intact on an allocation error.
        --next;
        const auto band = next->priority;
        next->message = std::move(_bands[band].back());
        _bands[band].pop_back();
      }
      _nonEmpty = previousMask;
      throw;
    }
    notifyWaiters();
  }
  void wake();
  void watch(const std::shared_ptr<std::atomic_uint64_t>& listener);
  void unwatch(const std::shared_ptr<std::atomic_uint64_t>& listener);
  void notifyWaiters();
  friend class Connection;
  friend class Consumer;
  friend class Session;
  friend class SessionDispatch;
  friend class Destination;

  DestinationMutex _mutex;
  std::array<std::deque<Message::Ptr>, QueueT::kBands> _bands;
  unsigned _nonEmpty{0};
  size_t _special = 0;
  std::atomic_bool _closed{false};
  std::vector<std::shared_ptr<Waiter>> _waiters;
  std::vector<std::weak_ptr<std::atomic_uint64_t>> _listeners;
  void notifyListeners();
};

class Destination : public std::enable_shared_from_this<Destination> {
 public:
  using Ptr = std::shared_ptr<Destination>;
  virtual ~Destination();

  destination::Type type() const;
  bool isQueueFamily() const;
  bool isTopicFamily() const;
  const std::string &name() const;
  std::string typeName() const;
  const std::string &uri() const;
  size_t hash() const;

  // Configure before starting delivery, as with the original redelivery API.
  void setRedeliveryPolicy(RedeliveryPolicy policy);
  RedeliveryPolicy redeliveryPolicy() const;

 private:
  using Storage = linear_storage::ConcurrentLinearStorage;

  struct Subscription {
    std::shared_ptr<DeliveryState> deliveries = std::make_shared<DeliveryState>();
    std::shared_ptr<Selector> selector;
    std::shared_ptr<Storage> storage;
    Poco::Path path;
    Poco::UUID connectionId;
    std::string clientId;
    bool noLocal{false};
    bool legacy{false};
    size_t active{0};  // guarded by the destination mutex
    std::weak_ptr<Consumer> legacyConsumer;
    std::optional<SubscriptionIdentity> sharedIdentity;

  };

  Destination(Exchange& exchange, destination::Type type, std::string name, Poco::Path path,
              Poco::UUID owner = {});
  void checkBroker(const Connection& connection) const;
  void checkOwner(const Poco::UUID& owner) const;
  void checkLive() const;

  Consumer::Ptr createConsumer(Session& session, std::shared_ptr<Selector> selector = {}, bool noLocal = false);
  Consumer::Ptr createDurableConsumer(Session& session, const std::string& clientId,
                                      const std::string& name, std::shared_ptr<Selector> selector);
  void deleteSubscription(const std::string& clientId, const std::string& name);
  Consumer::Ptr attach(Session& session, const std::shared_ptr<Subscription>& subscription, bool requireActive = false);
  void detach(Consumer& consumer);
  void retire(const std::shared_ptr<Subscription>& subscription, bool removeFiles);
  void shutdown();
  void deleteTemporary(const Poco::UUID& owner);

  void save(const Producer& producer, const Message& message, int64_t acceptedAt);
  struct Publication {
    Destination* destination;
    std::shared_ptr<DeliveryState> queue;
    std::shared_ptr<Storage> storage;
    Message::Ptr message;
    int32_t priority;
  };
  void preparePublication(const Producer& producer, const Message::Ptr& message,
                          std::vector<Publication>& outputs, int64_t now);

  Message::Ptr prepareCopy(const Producer& producer, Message::Ptr copy, int64_t number);
  bool matches(const Subscription& sub, const Producer& producer, const Message& message) const;
  void publish(const Producer& producer, const Message& message, int64_t acceptedAt);
  void deliver(const Subscription& subscription, Message::Ptr message, int64_t acceptedAt);
  static void persist(Storage& storage, Message& message);
  Poco::UUID storageKey(int64_t sequence) const;
  static Message::Ptr decodeStored(const std::vector<char>& bytes);
  void replay(const std::shared_ptr<DeliveryState>& queue, Storage& storage);
  void registerAttempt(Storage& storage, Message& message);
  void retry(const std::shared_ptr<DeliveryState>& queue, Storage* storage, Message::Ptr message,
             bool restoring = false);
  void enqueueOrSchedule(const std::shared_ptr<DeliveryState>& queue, Message::Ptr message);
  void enqueueOrSchedule(const std::shared_ptr<DeliveryState>& queue, Message::Ptr message, int64_t now);

  Exchange& _exchange;
  destination::Type _type;
  std::string _name;
  Poco::Path _path;
  Poco::UUID _owner;
  std::string _uri;
  size_t _hash;
  std::reference_wrapper<Poco::Logger> _logger;
  std::atomic_int64_t _messageCounter{0};
  std::array<char, 16> _storageEpoch{};
  std::atomic_bool _deleted{false};

  std::shared_ptr<DeliveryState> _queue;
  std::shared_ptr<Storage> _storage;
  DeliveryScheduler _scheduler;
  DestinationMutex _mutex;
  std::list<std::shared_ptr<Subscription>> _subscriptions;
  size_t _activeConsumers{0};
  std::mutex _legacyMutex;
  std::map<std::pair<std::string, std::string>, std::shared_ptr<Subscription>> _legacy;
  RedeliveryPolicy _redeliveryPolicy;

  friend class Exchange;
  friend class LocalTransactionJournal;
  friend class Producer;
  friend class Consumer;
  friend class Session;
  friend class QueueBrowser;
  friend class TemporaryDestination;
};

class TemporaryDestination final : public Destination {
 public:
  using Ptr = std::shared_ptr<TemporaryDestination>;
  void remove(Connection& connection);

 private:
  using Destination::Destination;
  friend class Exchange;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__DESTINATION_H_
