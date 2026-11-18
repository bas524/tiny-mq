#ifndef TINY_MQ__CONSUMER_H_
#define TINY_MQ__CONSUMER_H_

#include <deque>
#include <functional>
#include "ConcurrentLinearStorage.h"
#include "Message.h"
#include "Selector.h"

namespace tiny_mq {
class DeliveryState;
class Destination;
class DestinationMutex;
class Session;

enum class ReceiveBodyDisposition { empty, compatible, incompatible };
struct ReceiveBodyResult {
  ReceiveBodyDisposition disposition = ReceiveBodyDisposition::empty;
  Message::Ptr message;
};

class Consumer {
 public:
  using Ptr = std::shared_ptr<Consumer>;
  using MessageListener = std::function<void(Message::Ptr)>;

  const Poco::UUID& id() const;
  Message::Ptr receive();
  Message::Ptr receive(int64_t timeout);
  Message::Ptr receiveNoWait();
  ReceiveBodyResult receiveChecked(const std::function<bool(const Message&)>& predicate,
                                   int64_t usec_timeout = 10000000);
  void close();
  void setMessageListener(MessageListener listener);
  Message::Ptr recv(int64_t usec_timeout = 10000000);
  QueueT::producer_token_t getProducerToken();
  void acknowledgeOn(const Message& message);
  const Session& session() const;
  virtual ~Consumer();

 private:
  Consumer(Destination& destination, Session& session, std::shared_ptr<DeliveryState> queue,
           std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage,
           std::shared_ptr<Selector> selector = {}, bool durable = false);
  void checkReceiveState() const;
  void checkReceiveAdmission() const;
  void checkContendedReceiveAdmission();
  bool lockContendedReceive(std::unique_lock<DestinationMutex>& lock, int64_t timeout,
                            std::chrono::steady_clock::time_point& deadline);
  template<bool listener, bool checked>
  Message::Ptr receiveImpl(int64_t timeout, std::chrono::steady_clock::time_point deadline,
                           std::unique_lock<DestinationMutex> lock, const std::function<bool(const Message&)>* predicate,
                           ReceiveBodyDisposition* disposition = nullptr);
  bool dispatchOne();
  void track(const Message::Ptr& message) noexcept;
  void unlink(const Message& message);
  void redeliver(const Message& message, uint64_t batch = 0);
  void finishSession() noexcept;
  void clearSession() noexcept;
  void clearInFlight() noexcept;
  void flushPendingAcks();
  bool hasClaims() const;
  bool hasStorage(const Message& message) const {
    return message._storageTomId != std::numeric_limits<Poco::UInt32>::max();
  }
  enum ReceiveState : uint32_t { receiveIdle, receiveActive, closeWaiting };
  enum class CloseState : uint8_t { open, closing, closed };
  static constexpr uint32_t admissionSealed = uint32_t{1} << 31;
  bool receiveSealed() const { return _closeState.load(std::memory_order_acquire) != CloseState::open; }
  bool isClosed() const { return _closeState.load(std::memory_order_acquire) == CloseState::closed; }
  linear_storage::Record record(const Message& message) const;
  void acknowledgeStored(const Message& message);
  void acknowledge(const Message& message) {
    if (hasStorage(message)) [[unlikely]] acknowledgeStored(message);
  }

  Poco::UUID _uuid;
  std::shared_ptr<Destination> _destination;
  Session* _session;
  std::shared_ptr<DeliveryState> _queue;
  std::shared_ptr<linear_storage::ConcurrentLinearStorage> _storage;
  std::shared_ptr<Selector> _selector;
  std::reference_wrapper<Poco::Logger> _logger;
  std::deque<Message::Ptr> _transacted;
  // Count of claims backed by storage, including claims from a closed consumer.
  size_t _storedTransacted = 0;
  MessageListener _listener;
  std::mutex _listenerMutex;
  std::atomic_uint32_t _receiving{receiveIdle};
  std::atomic<CloseState> _closeState{CloseState::open};
  std::atomic_bool _callbackActive{false};
  size_t _pendingAcks = 0;
  // ADR-0005 gives the owning session exclusive access to these delivery links.
  Message::Ptr _inFlightHead;
  Message* _inFlightTail{nullptr};
  bool _durable;
  bool _countClaims;
  std::atomic_uint32_t _receiveAdmissionPins{0};

  friend class Destination;
  friend class DeliveryState;
  friend class Session;
  friend class SessionDispatch;
};
}  // namespace tiny_mq
#endif
