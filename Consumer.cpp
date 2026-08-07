//
// Created by Alexander Bychuk on 10.11.2021.
//

#include "Consumer.h"
#include "Producer.h"
#include "Destination.h"
#include "Session.h"
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <Poco/Format.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Timestamp.h>
#include <limits>
#include <optional>
#include <utility>
#include "LogTracer.h"

namespace tiny_mq {
Consumer::Consumer(Destination& destination, Session& session, std::shared_ptr<QueueT> queue, const Poco::UUID& uuid, Poco::Path path, std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage, std::shared_ptr<TransactionBuffer> transactionBuffer, std::shared_ptr<Selector> selector)
    : _uuid(uuid),
      _path(std::move(path)),
      _destination(destination),
      _destinationUri(destination.uri()),
      _queue(std::move(queue)),
      _token(*_queue),
      _logger(Poco::Logger::get(Poco::format("tiny_mq.consumer.%s", _uuid.toString()))),
      _session(session),
      _storage(std::move(storage)),
      _transactionBuffer(transactionBuffer),
      _selector(std::move(selector)) {
  TRACE(_logger);
  Poco::File dir(_path);
  dir.createDirectories();
  {
    Poco::FileOutputStream fo(_path.toString() + "/meta.info");
    fo << "consumer: " << _uuid.toString() << '\n';
    fo << "destination: " << _destination.get().name() << "[" << _destination.get().typeName() << "]" << '\n';
    fo << "session: " << _session.get().id() << '\n';
  }

  if (_session.get().acknowledgeMode() == Session::AcknowledgeMode::SESSION_TRANSACTED) {
    _transactQueue = std::make_shared<QueueT>();
  }
  poco_information(_logger.get(), Poco::format("create consumer[%s] for %s", _uuid.toString(), _destinationUri));
}

Consumer::~Consumer() {
  TRACE(_logger);
  flushPendingAcks();  // DUPS_OK: drain any batched acknowledgements before teardown
  try {
    rollback();
  } catch (...) {
    poco_error(_logger.get(), "rollback() threw in destructor — ignoring");
  }
  // Drop any never-recovered in-flight messages iteratively — letting
  // _inFlightHead's shared_ptr destructor unwind the chain on its own would
  // recurse one stack frame per in-flight message (see the CAUTION note on
  // Message::InFlightLink).
  clearInFlight();
  poco_information(_logger.get(), Poco::format("destroy consumer[%s] for %s", _uuid.toString(), _destinationUri));
}

const Poco::UUID& Consumer::id() const {
  TRACE(_logger);
  return _uuid;
}
Message::Ptr Consumer::recv(int64_t usec_timeout) {
  TRACE(_logger);
  Message::Ptr message;
  // Budget clock is started lazily on the first expired-message drop, so the
  // common path (return the first live message) never reads the clock. A negative
  // usec_timeout means "wait indefinitely" (moodycamel semantics) — pass it through
  // unchanged rather than clamping to a non-blocking poll.
  std::optional<Poco::Timestamp> started;
  int64_t remaining = usec_timeout;

  while (true) {
    message.reset();
    _queue->wait_dequeue_timed(_token, message, remaining);
    if (!message) return message;  // timed out with nothing left to deliver

    if (message->isPersistent()) {
      if (!message->_cachedStorageBytes.empty()) {
        // Fast path: bytes were cached by preparePush — skip storage round-trips.
        const auto& cached = message->_cachedStorageBytes;
        BytesVector bytesData(cached.size() > 1 ? cached.begin() + 1 : cached.end(),
                              cached.end());
        message->fromBytes(bytesData);
        message->_cachedStorageBytes.clear();
      } else {
        // Restart path: read from storage (no in-process cache available).
        auto record = _storage->record(message->uuid);
        if (record.tomId != std::numeric_limits<Poco::UInt32>::max()) {
          auto data = _storage->data(record);
          BytesVector bytesData(data.size() > 1 ? data.begin() + 1 : data.end(), data.end());
          message->fromBytes(bytesData);
        }
      }
    }

    // JMSExpiration (spec 44): an expired message is never delivered. Drop it —
    // removing any persistent copy so it cannot replay on restart — and keep
    // pulling within the caller's remaining timeout. Expiration is 0 (never) for
    // the vast majority of messages, so only read the clock when it is set —
    // this keeps the hot recv path clock-free.
    if (message->jmsHeaders.expiration != 0
        && message->isExpired(Poco::Timestamp().epochMicroseconds() / 1000)) {
      if (message->isPersistent()) {
        linear_storage::Record rec;
        if (message->_storageTomId != std::numeric_limits<Poco::UInt32>::max()) {
          rec.tomId  = message->_storageTomId;
          rec.offset = message->_storageOffset;
          message->uuid.copyTo(rec.header.uuid.data());  // let remove() drop the index entry
        } else {
          rec = _storage->record(message->uuid);
        }
        if (rec.tomId != std::numeric_limits<Poco::UInt32>::max()) {
          _storage->removeAsync(rec);
        }
      }
      poco_debug(_logger.get(),
                 Poco::format("drop expired message[%s] from %s",
                              message->uuid.toString(),
                              _destinationUri));
      if (usec_timeout >= 0) {
        if (!started) started.emplace();  // first drop: start the remaining-timeout budget
        remaining = usec_timeout - static_cast<int64_t>(started->elapsed());
        if (remaining <= 0) return Message::Ptr{};
      }
      continue;
    }

    // In SESSION_TRANSACTED mode, track each received message so rollback() can
    // redeliver and commit() can acknowledge.  Share the pointer — no copy needed.
    const auto mode = _session.get().acknowledgeMode();
    if (mode == Session::AcknowledgeMode::SESSION_TRANSACTED && _transactQueue) {
      _transactQueue->enqueue(message);
    } else if (mode != Session::AcknowledgeMode::AUTO_ACKNOWLEDGE) {
      // CLIENT_/DUPS_OK_/INDIVIDUAL_ACKNOWLEDGE: track as in-flight until
      // acknowledgeOn() (spec 23, Session.recover()). Share the pointer — no
      // copy needed. AUTO_ACKNOWLEDGE is deliberately excluded: per JMS 2.0
      // § 8.4.8 an AUTO_ACKNOWLEDGE message is considered acknowledged as
      // soon as recv() returns successfully to the caller, so there is
      // nothing left in flight for recover() to act on for this mode — and
      // since acknowledgeOn() is never called for AUTO_ACKNOWLEDGE (that is
      // this API's existing convention; see BenchmarkTest's
      // AutoAck_*_RoundTrip and ClientAckTest.testAutoAcknowledge), tracking
      // it here would grow _inFlight without bound for the lifetime of the
      // consumer — a real memory leak, confirmed by a ~29% CPU regression on
      // AutoAck_NonPersistent_RoundTrip during perf-check before this guard
      // was added.
      // Intrusive append at tail (see Message::InFlightLink).
      if (_inFlightTail) {
        _inFlightTail->_inFlightLink.next = message;
        message->_inFlightLink.prev = _inFlightTail;
      } else {
        // Defense in depth (review round 2, B2 item 3): a message reaching
        // here should already have a null link — either freshly delivered,
        // or (since the Message copy ctor no longer copies this field) a
        // copy made for fan-out — but a stale prev surviving into an
        // empty-chain link would silently break the "prev == nullptr iff
        // head" invariant relied on elsewhere. Cheap to assert by construction.
        message->_inFlightLink.prev = nullptr;
        _inFlightHead = message;
      }
      _inFlightTail = message.get();
      // Spec 23 round 3 (B2 fix): tag the message with its owning consumer so
      // acknowledgeOn() can tell "in my chain" from "in someone else's chain"
      // without inferring it from prev/head alone (see Message::InFlightLink::owner).
      message->_inFlightLink.owner = this;
    }

    poco_debug(_logger.get(),
               Poco::format("recv message[%?d][%s] from %s",
                            message->_number,
                            message->uuid.toString(),
                            _destinationUri));
    return message;
  }
}
Message::Ptr Consumer::preparePush(int64_t number, const Producer& producer, const Message& message) {
  TRACE(_logger);

  Message::Ptr copyMessage = message.copy();
  if (copyMessage->uuid.isNull()) {
    copyMessage->uuid = _session.get().createRandomUUID();
  }

  copyMessage->_number = number;
  
  // Check if this is a transactional message
  std::string transactionId = producer.transactionId();
  bool isTransactional = !transactionId.empty();
  
  if (copyMessage->isPersistent()) {
    // Stored format: [1-byte type][toBytes() payload]
    // The type byte lets replayStoredMessages() recreate the right shell on restart.
    auto bytesData = copyMessage->toBytes();
    std::vector<char> data;
    data.reserve(1 + bytesData.size());
    data.push_back(static_cast<char>(copyMessage->type()));
    data.insert(data.end(), bytesData.begin(), bytesData.end());

    // Cache the bytes so recv() can skip the storage read round-trips.
    copyMessage->_cachedStorageBytes = data;

    if (isTransactional && _transactionBuffer) {
      _transactionBuffer->addMessage(transactionId, copyMessage->uuid, data);
      poco_debug(_logger.get(),
                Poco::format("Buffered message[%s] in transaction %s",
                            copyMessage->uuid.toString(), transactionId));
    } else {
      auto rec = _storage->append(copyMessage->uuid, data);
      // Cache the record location so acknowledgeOn() can skip the UUID lookup.
      copyMessage->_storageTomId  = rec.tomId;
      copyMessage->_storageOffset = rec.offset;
    }
  }
  
  return copyMessage;
}
void Consumer::push(int64_t number, const Producer& producer, const Message& message) {
  TRACE(_logger);
  if (_selector && !_selector->matches(message)) return;
  Message::Ptr msg = preparePush(number, producer, message);
  if (producer.transactionId().empty()) {
    if (msg->jmsHeaders.deliveryTime != 0) {
      // JMS 2.0 § 7.8 (spec 13): a delayed message is invisible to consumers
      // until due — route through the destination's scheduler instead of the
      // queue directly. Rare path; deliveryTime is 0 for the vast majority of
      // messages, so this check is the only cost on the hot (non-delayed) path.
      _destination.get().enqueueOrSchedule(_queue, std::move(msg));
    } else if (producer._token) {
      // Producer token is only valid for queue-type destinations; use tokenless
      // enqueue for topics where no token was created.
      _queue->enqueue(producer.token(), std::move(msg));
    } else {
      _queue->enqueue(std::move(msg));
    }
  } else {
    producer.transactQueue().enqueue(std::move(msg));
  }
  poco_debug(
      _logger.get(),
      Poco::format("save message[%?d][%s] to %s", number, message.uuid.toString(), _destinationUri));
}

void Consumer::commit() {
  TRACE(_logger);
  if (!_transactQueue) return;  // not in SESSION_TRANSACTED mode
  Message::Ptr msg;
  while (_transactQueue->try_dequeue(msg)) {
    if (msg && msg->isPersistent()) {
      if (msg->_storageTomId != std::numeric_limits<Poco::UInt32>::max()) {
        // Fast path: use cached record location, fire-and-forget.
        linear_storage::Record rec;
        rec.tomId  = msg->_storageTomId;
        rec.offset = msg->_storageOffset;
        msg->uuid.copyTo(rec.header.uuid.data());  // let remove() drop the index entry
        _storage->removeAsync(rec);
      } else {
        // Restart / transacted path: lookup then remove asynchronously.
        auto record = _storage->record(msg->uuid);
        if (record.tomId != std::numeric_limits<Poco::UInt32>::max()) {
          _storage->removeAsync(record);
        }
      }
    }
  }
  // Queue is now empty; reuse the existing object instead of reallocating.
}

void Consumer::rollback() {
  TRACE(_logger);
  // Logs the cached _destinationUri, not a live _destination.get() call:
  // this method also runs from ~Consumer() (drains any un-acked transacted
  // messages on teardown), where the live Destination is not safe to touch
  // for logging purposes — see the comment on _destinationUri in Consumer.h.
  if (!_transactQueue) return;  // not in SESSION_TRANSACTED mode
  Message::Ptr msg;
  do {
    msg.reset();
    _transactQueue->wait_dequeue_timed(msg, 1000);
    if (msg != nullptr) {
      _queue->enqueue(msg);
      poco_trace(_logger.get(),
                 Poco::format("rollback message[%?d][%s] to %s",
                              msg->number(), msg->uuid.toString(), _destinationUri));
    }
  } while (msg != nullptr);
  // Queue drained in place; no reallocation needed.
}


void Consumer::recover() {
  TRACE(_logger);
  // Detach the chain up front. Re-enqueuing below makes these messages
  // deliverable again; nothing here re-enters recv() synchronously
  // (Session/Consumer are single-threaded per ADR-0005), but leaving stale
  // links live while iterating would be fragile if that ever changes.
  Message::Ptr node = std::move(_inFlightHead);
  _inFlightTail = nullptr;
  // Only read by the poco_debug() call below, which compiles away entirely
  // in release builds (POCO_LOG_DEBUG off) — hence [[maybe_unused]].
  [[maybe_unused]] size_t requeued = 0;
  while (node) {
    // Extract `next` and clear this node's own links before touching
    // anything else: leaving `node`'s next non-null while we still hold a
    // shared_ptr to `node` (in this loop, and now also implicitly via
    // `next`) doesn't recurse — but doing this extraction up front is what
    // keeps the walk O(1)-per-node instead of O(n) per node, and matches
    // clearInFlight()'s teardown pattern so both stay obviously consistent.
    // See the CAUTION note on Message::InFlightLink.
    Message::Ptr next = std::move(node->_inFlightLink.next);
    node->_inFlightLink.prev = nullptr;
    node->_inFlightLink.owner = nullptr;  // no longer linked into this chain

    node->jmsHeaders.redelivered = true;
    ++node->jmsHeaders.deliveryCount;
    // Keep the serialized cache in sync with the header mutation just made
    // (ADR-0008): recv() already consumed and cleared _cachedStorageBytes on
    // this message's first delivery, so without this the *next* recv() would
    // fall back to the stale on-disk bytes and silently drop redelivered/
    // deliveryCount. Never touches the on-disk record or any durable-
    // subscriber storage — the message was already persisted there on its
    // first delivery; recover() only re-arms in-memory delivery.
    node->refreshCachedStorageBytes();
    // Tokenless enqueue — no producer context here, same as Consumer::rollback().
    _queue->enqueue(node);
    poco_trace(_logger.get(),
               Poco::format("recover: redeliver message[%?d][%s] to %s",
                            node->number(), node->uuid.toString(), _destinationUri));
    ++requeued;
    node = std::move(next);
  }
  poco_debug(_logger.get(),
             Poco::format("recover: requeued %z message(s) to %s",
                          requeued, _destinationUri));
}

void Consumer::clearInFlight() noexcept {
  // Same iterative unlink pattern as recover(), without the redelivery side
  // effects — used by ~Consumer() to drop whatever recover() never got to.
  // Required so the chain doesn't destroy itself recursively; see the
  // CAUTION note on Message::InFlightLink.
  //
  // Deliberately no TRACE()/poco_* call here (B3, ADR-0006): this function
  // runs from ~Consumer(), where a throwing call (Poco::format allocation,
  // logging channel I/O) is std::terminate, not a caught exception. Every
  // statement below is a shared_ptr move or a raw-pointer assignment, neither
  // of which can throw — that is what makes noexcept an honest contract here,
  // not just a promise.
  Message::Ptr node = std::move(_inFlightHead);
  _inFlightTail = nullptr;
  while (node) {
    Message::Ptr next = std::move(node->_inFlightLink.next);
    node->_inFlightLink.prev = nullptr;
    node->_inFlightLink.owner = nullptr;
    node = std::move(next);
  }
}

void Consumer::flushPendingAcks() {
  TRACE(_logger);
  for (const auto& rec : _pendingAcks) {
    _storage->removeAsync(rec);
  }
  _pendingAcks.clear();
}

QueueT& Consumer::transactQueue() const { return *_transactQueue; }

Destination& Consumer::destination() const { return _destination.get(); }

QueueT::producer_token_t Consumer::getProducerToken() {
  TRACE(_logger);
  return QueueT::producer_token_t(*_queue);
}
const Session& Consumer::session() const { return _session; }

void Consumer::acknowledgeOn(const Message& message) {
  TRACE(_logger);
  // Pin `message` alive for the whole function (round-3 review, B4). See the
  // comment where `self` is assigned below for why this is necessary and not
  // just defensive.
  Message::Ptr self;
  const auto mode = _session.get().acknowledgeMode();
  if (mode != Session::AcknowledgeMode::SESSION_TRANSACTED) {
    // Drop from in-flight tracking (spec 23) regardless of persistence — an
    // acknowledged message must never be redelivered by a later recover().
    // O(1) intrusive unlink (see Message::InFlightLink): splice `message`
    // out of Consumer's chain using its own prev/next links, no scan (a
    // scan-plus-shift made acknowledging a batch of n messages O(n^2) — see
    // the container choice note in Consumer.h).
    //
    // Ownership test (review round 2, B2): the previous "tracked" test —
    // non-null prev, or identity against _inFlightHead — answered "is this
    // message linked into *some* chain", not "into *my* chain". A message
    // acknowledged on the wrong consumer of the same queue (a legitimate,
    // undetected-by-the-app mistake) has a non-null prev pointing into a
    // *different* consumer's chain; the old test spliced it out of that
    // consumer's chain from here, corrupting it (round-2 ZZ5/ZZ1/ZZ2/ZZ4).
    // owner is set exactly when a message is linked, by the same Consumer
    // that links it (Consumer::recv()), so comparing it against `this`
    // answers the right question directly — an ack on a foreign message
    // becomes an honest no-op, as it was back when this was a linear-scan
    // std::vector local to `this` consumer (round 1).
    if (message._inFlightLink.owner == this) {
      Message* prevPtr = message._inFlightLink.prev;
      // Copy the chain's *owning* reference to `message` into `self` (the
      // function-scope variable declared above) before the two assignments
      // below drop that reference (prevPtr->_inFlightLink.next or
      // _inFlightHead). If that chain link is the last surviving
      // Message::Ptr — a caller is free to drop its own Ptr and keep
      // acknowledging via a bare Message& (see the class comment on
      // Message::InFlightLink, which promises exactly this) — dropping it
      // destroys `message` immediately, while this function still writes to
      // it (below) and reads from it (persistence bookkeeping, logging) for
      // the rest of its body: heap-use-after-free (round-3 review, B4).
      // `self` keeps `message` alive until it goes out of scope at the end
      // of acknowledgeOn — one extra atomic refcount round-trip, paid only
      // on the already-tracked ack path.
      self = prevPtr ? prevPtr->_inFlightLink.next : _inFlightHead;
      Message::Ptr nextOwning = std::move(message._inFlightLink.next);
      Message* nextPtr = nextOwning.get();

      if (prevPtr) {
        prevPtr->_inFlightLink.next = std::move(nextOwning);  // drops the chain's ref to `message`
      } else {
        _inFlightHead = std::move(nextOwning);  // `message` was head
      }
      if (nextPtr) {
        nextPtr->_inFlightLink.prev = prevPtr;
      } else {
        _inFlightTail = prevPtr;  // `message` was tail
      }
      message._inFlightLink.prev = nullptr;  // next already moved-from (null)
      message._inFlightLink.owner = nullptr;
    }

    if (message.isPersistent()) {
      // Resolve the storage record (cached fast path, else UUID lookup).
      linear_storage::Record rec;
      if (message._storageTomId != std::numeric_limits<Poco::UInt32>::max()) {
        rec.tomId  = message._storageTomId;
        rec.offset = message._storageOffset;
        // Carry the uuid so remove() can drop the index entry without an extra
        // on-disk header read on this hot path.
        message.uuid.copyTo(rec.header.uuid.data());
      } else {
        rec = _storage->record(message.uuid);
      }
      if (rec.tomId != std::numeric_limits<Poco::UInt32>::max()) {
        if (mode == Session::AcknowledgeMode::DUPS_OK_ACKNOWLEDGE) {
          // Lazy/batched: accumulate and flush at the batch threshold. On crash
          // before flush, unremoved records replay as duplicates — the mode's contract.
          constexpr size_t kDupsOkBatch = 100;
          _pendingAcks.push_back(rec);
          if (_pendingAcks.size() >= kDupsOkBatch) {
            flushPendingAcks();
          }
        } else {
          _storage->removeAsync(rec);
        }
      }
    }
  }
  // SESSION_TRANSACTED: recv() already tracks messages in _transactQueue;
  // acknowledgement happens implicitly on session.commit().
  poco_debug(_logger.get(),
             Poco::format("ack on message[%?d][%s] to %s",
                          message._number, message.uuid.toString(), _destinationUri));
}
}  // namespace tiny_mq