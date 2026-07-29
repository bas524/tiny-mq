//
// Created by Alexander Bychuk on 30.12.2025.
//

#include "ConcurrentLinearStorage.h"
#include <Poco/RunnableAdapter.h>
#include <Poco/Timestamp.h>
#include <cstring>
#include "LogTracer.h"

namespace linear_storage {

std::string OperationIdToString(OperationId operationId) {
  switch (operationId) {
    case OperationId::GET_RECORD_BY_TOM_OFFSET: return "GET_RECORD_BY_TOM_OFFSET";
    case OperationId::GET_RECORD_BY_UUID:       return "GET_RECORD_BY_UUID";
    case OperationId::GET_DATA_BY_RECORD:       return "GET_DATA_BY_RECORD";
    case OperationId::APPEND:                   return "APPEND";
    case OperationId::REMOVE:                   return "REMOVE";
    case OperationId::STOP:                     return "STOP";
    case OperationId::SCAN:                     return "SCAN";
    case OperationId::APPEND_BATCH:             return "APPEND_BATCH";
    default:                                    return "UNKNOWN";
  }
}
ConcurrentLinearStorage::ConcurrentLinearStorage(const Poco::UUID& id, Poco::Path basePath)
    : _linearStorage(id, std::move(basePath)), _logger(Poco::Logger::get(Poco::format("tiny_mq.cuncurrent_storge.%s", id))) {
  TRACE(_logger);
}
ConcurrentLinearStorage::~ConcurrentLinearStorage() { stop(); }
Record ConcurrentLinearStorage::append(const Poco::UUID& uuid, const std::vector<char>& data) {
  TRACE(_logger);
  Operation operation;
  operation.id = OperationId::APPEND;
  operation.uuid = uuid;
  operation.data = data;
  _operations.enqueue(&operation);
  operation.event.wait();
  return operation.record;
}
Record ConcurrentLinearStorage::record(Poco::UInt32 tomId, Poco::UInt64 offset) const {
  TRACE(_logger);
  Operation operation;
  operation.id = OperationId::GET_RECORD_BY_TOM_OFFSET;
  operation.tomId = tomId;
  operation.offset = offset;
  _operations.enqueue(&operation);
  operation.event.wait();
  return operation.record;
}
Record ConcurrentLinearStorage::record(const Poco::UUID& uuid) const {
  TRACE(_logger);
  Operation operation;
  operation.id = OperationId::GET_RECORD_BY_UUID;
  operation.uuid = uuid;
  _operations.enqueue(&operation);
  operation.event.wait();
  return operation.record;
}
std::vector<char> ConcurrentLinearStorage::data(const Record& record) const {
  TRACE(_logger);
  Operation operation;
  operation.id = OperationId::GET_DATA_BY_RECORD;
  operation.record = record;
  _operations.enqueue(&operation);
  operation.event.wait();
  return operation.data;
}
bool ConcurrentLinearStorage::remove(const Record& record) {
  TRACE(_logger);
  Operation operation;
  operation.id = OperationId::REMOVE;
  operation.record = record;
  _operations.enqueue(&operation);
  operation.event.wait();
  return static_cast<bool>(operation.record.header.deleted);
}

void ConcurrentLinearStorage::removeAsync(const Record& record) {
  if (record.tomId == std::numeric_limits<Poco::UInt32>::max()) return;
  auto* op = new Operation;
  op->id = OperationId::REMOVE;
  op->record = record;
  op->async = true;
  _operations.enqueue(op);
}

void ConcurrentLinearStorage::appendBatch(std::vector<std::pair<Poco::UUID, std::vector<char>>> items) {
  if (items.empty()) return;
  Operation operation;
  operation.id = OperationId::APPEND_BATCH;
  operation.batchItems = std::move(items);
  _operations.enqueue(&operation);
  operation.event.wait();
}
std::vector<std::pair<Record, std::vector<char>>> ConcurrentLinearStorage::scan() const {
  TRACE(_logger);
  Operation operation;
  operation.id = OperationId::SCAN;
  _operations.enqueue(&operation);
  operation.event.wait();
  return std::move(operation.scanResult);
}
void ConcurrentLinearStorage::start() {
  TRACE(_logger);
  if (_isRunning == false) {
    _thread.start(*this);
  }
}
void ConcurrentLinearStorage::run() {
  TRACE(_logger);
  _isRunning = true;
  _lastSweepUs = Poco::Timestamp().epochMicroseconds();
  while (_isRunning) {
    Operation* operation = nullptr;
    // Wait at most one sweep interval so the deadline check below still fires when
    // the queue is idle; under load an operation arrives well before the timeout.
    _operations.wait_dequeue_timed(operation, _sweepIntervalUs.load());

    if (operation != nullptr) {
      switch (operation->id) {
        case OperationId::GET_RECORD_BY_TOM_OFFSET:
          operation->record = _linearStorage.record(operation->tomId, operation->offset);
          break;
        case OperationId::GET_RECORD_BY_UUID:
          operation->record = _linearStorage.record(operation->uuid);
          break;
        case OperationId::GET_DATA_BY_RECORD:
          operation->data = _linearStorage.data(operation->record);
          break;
        case OperationId::APPEND:
          operation->record = _linearStorage.append(operation->uuid, operation->data);
          break;
        case OperationId::REMOVE:
          _linearStorage.remove(operation->record);
          break;
        case OperationId::APPEND_BATCH:
          for (auto& [uuid, data] : operation->batchItems) {
            _linearStorage.append(uuid, data);
          }
          break;
        case OperationId::SCAN:
          operation->scanResult = _linearStorage.scan();
          break;
        case OperationId::STOP:
          _isRunning = false;
          break;
      }

      // Async operations are heap-allocated; delete instead of signalling.
      if (operation->async) {
        delete operation;
      } else {
        operation->event.set();
      }
    }

    // JMSExpiration sweep (spec 44) on a deadline — cadence does NOT depend on the
    // queue going fully idle (B1). sweepExpired() is chunked (B2), so this stays
    // bounded even under a continuous operation stream.
    const int64_t nowUs = Poco::Timestamp().epochMicroseconds();
    if (nowUs - _lastSweepUs >= _sweepIntervalUs.load()) {
      sweepExpired();
      _lastSweepUs = nowUs;
    }
  }
}
void ConcurrentLinearStorage::stop() {
  TRACE(_logger);
  if (_isRunning) {
    Operation operation;
    operation.id = OperationId::STOP;
    _operations.enqueue(&operation);
    operation.event.wait();
    _thread.join();
  }
}
bool ConcurrentLinearStorage::isRunning() { return _isRunning; }

void ConcurrentLinearStorage::setSweepIntervalMicros(int64_t us) {
  if (us > 0) _sweepIntervalUs = us;
}

void ConcurrentLinearStorage::sweepExpired() {
  // Runs on the worker thread, so it may touch _linearStorage directly without
  // racing the operation dispatch in run().
  //
  // Stored record layout: [1-byte type][0x02 message payload]. JMSExpiration is an
  // int64 at a fixed offset inside the 0x02 header block:
  //   type(1) + magic(1) + number(8) + uuid(16) + reliability(1) + timestamp(8).
  // Read only that fixed prefix per record (not the whole payload), and cap the
  // number of records per tick so a large store never monopolises the worker.
  constexpr size_t kMagicOffset      = 1;
  constexpr size_t kExpirationOffset = 1 + 1 + sizeof(int64_t) + 16 + 1 + sizeof(int64_t);  // 35
  constexpr size_t kPrefixLen        = kExpirationOffset + sizeof(int64_t);                  // 43
  constexpr size_t kSweepBudget      = 4096;  // records inspected per tick

  auto batch = _linearStorage.scanPrefix(kPrefixLen, kSweepBudget, _sweepCursor);
  if (batch.empty()) return;

  const int64_t nowMs = Poco::Timestamp().epochMicroseconds() / 1000;
  for (auto& [record, prefix] : batch) {
    if (prefix.size() < kPrefixLen) continue;                          // too short to carry headers
    if (static_cast<uint8_t>(prefix[kMagicOffset]) != 0x02) continue;  // only 0x02 carries expiration
    int64_t expiration = 0;
    std::memcpy(&expiration, prefix.data() + kExpirationOffset, sizeof(int64_t));
    if (expiration != 0 && nowMs >= expiration) {
      Record rec = record;  // remove() mutates the header (deleted flag)
      _linearStorage.remove(rec);
    }
  }
}
}  // namespace linear_storage