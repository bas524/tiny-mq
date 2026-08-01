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
  poco_information(_logger.get(), Poco::format("create consumer[%s] for %s", _uuid.toString(), _destination.get().uri()));
}

Consumer::~Consumer() {
  TRACE(_logger);
  flushPendingAcks();  // DUPS_OK: drain any batched acknowledgements before teardown
  try {
    rollback();
  } catch (...) {
    poco_error(_logger.get(), "rollback() threw in destructor — ignoring");
  }
  poco_information(_logger.get(), Poco::format("destroy consumer[%s] for %s", _uuid.toString(), _destination.get().uri()));
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
                 Poco::format("drop expired message[%s] from %s://%s",
                              message->uuid.toString(),
                              _destination.get().typeName(),
                              _destination.get().name()));
      if (usec_timeout >= 0) {
        if (!started) started.emplace();  // first drop: start the remaining-timeout budget
        remaining = usec_timeout - static_cast<int64_t>(started->elapsed());
        if (remaining <= 0) return Message::Ptr{};
      }
      continue;
    }

    // In SESSION_TRANSACTED mode, track each received message so rollback() can
    // redeliver and commit() can acknowledge.  Share the pointer — no copy needed.
    if (_session.get().acknowledgeMode() == Session::AcknowledgeMode::SESSION_TRANSACTED
        && _transactQueue) {
      _transactQueue->enqueue(message);
    }

    poco_debug(_logger.get(),
               Poco::format("recv message[%?d][%s] from %s://%s",
                            message->_number,
                            message->uuid.toString(),
                            _destination.get().typeName(),
                            _destination.get().name()));
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
    // Producer token is only valid for queue-type destinations; use tokenless
    // enqueue for topics where no token was created.
    if (producer._token) {
      _queue->enqueue(producer.token(), std::move(msg));
    } else {
      _queue->enqueue(std::move(msg));
    }
  } else {
    producer.transactQueue().enqueue(std::move(msg));
  }
  poco_debug(
      _logger.get(),
      Poco::format("save message[%?d][%s] to %s://%s", number, message.uuid.toString(), _destination.get().typeName(), _destination.get().name()));
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
  if (!_transactQueue) return;  // not in SESSION_TRANSACTED mode
  Message::Ptr msg;
  do {
    msg.reset();
    _transactQueue->wait_dequeue_timed(msg, 1000);
    if (msg != nullptr) {
      _queue->enqueue(msg);
      poco_trace(_logger.get(),
                 Poco::format("rollback message[%?d][%s] to %s://%s",
                              msg->number(), msg->uuid.toString(),
                              _destination.get().typeName(), _destination.get().name()));
    }
  } while (msg != nullptr);
  // Queue drained in place; no reallocation needed.
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
  const auto mode = _session.get().acknowledgeMode();
  if (mode != Session::AcknowledgeMode::SESSION_TRANSACTED) {
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
             Poco::format("ack on message[%?d][%s] to %s://%s",
                          message._number, message.uuid.toString(),
                          _destination.get().typeName(), _destination.get().name()));
}
}  // namespace tiny_mq