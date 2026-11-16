//
// Created by Alexander Bychuk on 30.12.2025.
//

#ifndef TINY_MQ_CONCURRENTLINEARSTORAGE_H
#define TINY_MQ_CONCURRENTLINEARSTORAGE_H

#include <Poco/Logger.h>
#include "LinearStorage.h"
#include "MessageProperty.h"

#include <Poco/Runnable.h>
#include <Poco/Thread.h>
#include <external/concurrent_queue/BlockingConcurrentQueueHeader.h>
#include <mutex>

namespace tiny_mq { class LocalTransactionJournal; }

namespace linear_storage {
enum class OperationId {
  GET_RECORD_BY_TOM_OFFSET = 0,
  GET_RECORD_BY_UUID = 1,
  GET_DATA_BY_RECORD = 2,
  APPEND = 3,
  REMOVE = 4,
  SCAN = 6,
  APPEND_BATCH = 7  // persist multiple records in one worker dispatch
};

struct Operation {
  OperationId id;
  Poco::UUID uuid;
  Poco::UInt32 tomId{0};
  Poco::UInt64 offset{0};
  std::vector<char> data;
  Record record;
  std::vector<std::pair<Record, std::vector<char>>> scanResult;
  // Batch-append payload (APPEND_BATCH only)
  std::vector<std::pair<Poco::UUID, std::vector<char>>> batchItems;
  // When true the operation is heap-allocated; the worker deletes it instead of
  // signalling the event (fire-and-forget / async path).
  bool async{false};
  Poco::Event event;
};

class ConcurrentLinearStorage: Poco::Runnable {
  friend class tiny_mq::LocalTransactionJournal;
  Storage _linearStorage;
  std::mutex _storageMutex;
  mutable std::mutex _lifecycleMutex;
  mutable moodycamel::BlockingConcurrentQueue<Operation*> _operations;
  moodycamel::ProducerToken _operationProducer{_operations};
  Poco::Thread _thread;
  std::atomic<bool> _isRunning{false};
  // Written with both mutexes held. Readers hold the mutex for their operation path.
  bool _acceptingOperations{false};
  std::atomic<int64_t> _sweepIntervalUs{1'000'000};  // JMSExpiration sweep cadence (spec 44), default 1 s
  int64_t _lastSweepUs{0};                           // worker-only: deadline for the next sweep (B1)
  linear_storage::SweepCursor _sweepCursor{};        // worker-only: resume point for chunked sweeps (B2)
  std::reference_wrapper<Poco::Logger> _logger;

public:
  ConcurrentLinearStorage(const Poco::UUID &id, Poco::Path basePath);
  ~ConcurrentLinearStorage() override;

  Record appendDelivery(const Poco::UUID &uuid, const std::vector<char> &data);
  bool removeDelivery(Record record);
  Record append(const Poco::UUID &uuid, const std::vector<char> &data);
  // Append multiple records in a single worker dispatch (amortises round-trip cost).
  void appendBatch(std::vector<std::pair<Poco::UUID, std::vector<char>>> items);
  [[nodiscard]] Record record(Poco::UInt32 tomId, Poco::UInt64 offset) const;
  [[nodiscard]] Record record(const Poco::UUID &uuid) const;
  [[nodiscard]] std::vector<char> data(const Record &record) const;
  bool remove(const Record &record);
  // Fire-and-forget remove: does not block the caller.
  void removeAsync(const Record &record);
  [[nodiscard]] std::vector<std::pair<Record, std::vector<char>>> scan() const;

  void start();
  void run() override;
  void stop();
  bool isRunning();
  // JMSExpiration sweep cadence (spec 44); ignored if not > 0. Default 1 s.
  void setSweepIntervalMicros(int64_t us);

 private:
  void enqueue(Operation* operation) const;
  // Runs on the worker thread with _storageMutex held.
  void sweepExpired();
};

}
#endif  // TINY_MQ_CONCURRENTLINEARSTORAGE_H
