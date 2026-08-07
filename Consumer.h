//
// Created by Alexander Bychuk on 10.11.2021.
//

#ifndef TINY_MQ__CONSUMER_H_
#define TINY_MQ__CONSUMER_H_

#include <memory>
#include <string>
#include <Poco/Path.h>
#include <Poco/Logger.h>
#include "Message.h"
#include "ConcurrentLinearStorage.h"
#include "TransactionBuffer.h"
#include "Selector.h"

namespace tiny_mq {
class Destination;
class Producer;
class Session;
class Consumer {
  Poco::UUID _uuid;
  Poco::Path _path;
  std::reference_wrapper<Destination> _destination;
  // Cached at construction (Destination is unconditionally alive there) and
  // used for every *logging* reference to the destination's identity from
  // then on, including from ~Consumer() and from rollback() when it runs as
  // part of ~Consumer()'s teardown. This is the fix for a preexisting
  // heap-use-after-free found while adding spec 23's ASan CI gate (round 4).
  //
  // Mechanism (confirmed from the ASan stack, not guessed): a Consumer can
  // outlive its Destination. Session::_consumers co-owns the Consumer, so
  // when ~Destination() finishes, the Consumer is still alive and its
  // _destination reference dangles for the rest of its life. ~Consumer()
  // then logged _destination.get().uri() and read freed memory. Note that
  // ~Destination() deliberately clears _consumers first — the ordering
  // inside that destructor is not the problem; the shared ownership is.
  //
  // Caching the string at construction removes the dependency for logging.
  // The underlying lifetime issue (Consumer holding a raw reference to an
  // object it can outlive) is NOT fixed here — see
  // tasks/memory-safety/01-consumer-outlives-destination.md.
  // Functional (non-logging) uses of _destination — enqueueOrSchedule(),
  // the destination() accessor — still go through the live reference.
  std::string _destinationUri;
  std::shared_ptr<QueueT> _queue;
  QueueT::consumer_token_t _token;
  std::reference_wrapper<Poco::Logger> _logger;
  std::reference_wrapper<Session> _session;
  std::shared_ptr<QueueT> _transactQueue;
  std::shared_ptr<linear_storage::ConcurrentLinearStorage> _storage;
  std::shared_ptr<TransactionBuffer> _transactionBuffer;
  std::shared_ptr<Selector> _selector;  // nullptr = no filter (match all)
  // DUPS_OK_ACKNOWLEDGE: storage removals accumulate here and flush in batches.
  std::vector<linear_storage::Record> _pendingAcks;
  // Spec 23 (Session.recover()): messages delivered to the app but not yet
  // acknowledged, for non-transactional ack modes only (SESSION_TRANSACTED
  // tracks its own in-flight set in _transactQueue and uses rollback()
  // instead). Linked in recv(), unlinked in acknowledgeOn(), drained and
  // redelivered — in original order — by recover(). No lock: per ADR-0005 a
  // Session/Consumer has thread affinity to a single caller thread.
  //
  // Intrusive doubly-linked list via Message::InFlightLink (Message's
  // _inFlightLink.next/.prev — see the comment there for the two rejected
  // alternatives — a plain std::vector, and std::list<Message::Ptr> — and
  // their measured cost). _inFlightHead owns the chain; _inFlightTail is a
  // non-owning back-pointer enabling O(1) tail insertion. Empty iff both are
  // null/nullptr.
  Message::Ptr _inFlightHead;
  Message* _inFlightTail{nullptr};

 public:
  using Ptr = std::shared_ptr<Consumer>;
  const Poco::UUID &id() const;
  Message::Ptr recv(int64_t usec_timeout = 10000000);
  QueueT::producer_token_t getProducerToken();
  void acknowledgeOn(const Message &message);
  const Session &session() const;
  virtual ~Consumer();

 private:
  explicit Consumer(Destination &destination, Session &session, std::shared_ptr<QueueT> queue, const Poco::UUID &uuid, Poco::Path path, std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage, std::shared_ptr<TransactionBuffer> transactionBuffer, std::shared_ptr<Selector> selector = nullptr);
  Message::Ptr preparePush(int64_t number, const Producer &producer, const Message &message);
  void push(int64_t number, const Producer &producer, const Message &message);
  void commit();
  void rollback();
  // Session.recover() (spec 23): requeue every in-flight (received, not yet
  // acknowledged) message onto this consumer's queue, in original receive
  // order, with headers.redelivered set and headers.deliveryCount incremented.
  // Never touches durable-subscriber storage — the messages are already
  // persisted there (or in the destination's own storage) from their first
  // delivery; recover() only re-arms in-memory delivery.
  void recover();
  // Unlink and drop every in-flight message without redelivering (used by
  // ~Consumer() for messages recover() never got to). Walks the intrusive
  // chain one node at a time, extracting `next` before dropping the current
  // node's shared_ptr — required so the chain doesn't destroy itself
  // recursively (Message::InFlightLink::next owns the next node; letting
  // _inFlightHead simply go out of scope would recurse one stack frame per
  // in-flight message). See the CAUTION note on Message::InFlightLink.
  //
  // Spec 23 round 3 (B3, ADR-0006): called only from ~Consumer(), where a
  // throwing call is std::terminate. The body below touches only shared_ptr
  // moves and raw-pointer assignment (none of which can throw), and
  // deliberately has no TRACE()/logging call — unlike recover(), which is
  // not called from a destructor and may log freely. noexcept makes that
  // contract enforceable by the compiler rather than by convention.
  void clearInFlight() noexcept;
  void flushPendingAcks();
  QueueT &transactQueue() const;
  Destination &destination() const;
  
  friend class Destination;
  friend class Session;
};
}  // namespace tiny_mq
#endif  // TINY_MQ__CONSUMER_H_
