//
// Created by Alexander Bychuk on 30.12.2025.
//

#include "ConcurrentLinearStorage.h"
#include <Poco/RunnableAdapter.h>
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
  while (_isRunning) {
    Operation* operation = nullptr;
    _operations.wait_dequeue(operation);
    if (operation == nullptr) continue;

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
}  // namespace linear_storage