#ifndef TINY_MQ_LOCAL_TRANSACTION_JOURNAL_H
#define TINY_MQ_LOCAL_TRANSACTION_JOURNAL_H

#include "ConcurrentLinearStorage.h"
#include <filesystem>
#include <optional>
#ifdef _WIN32
#include <fstream>
#endif

namespace tiny_mq {
class Consumer;
class Destination;
class Exchange;
class Session;

// Internal coordinator for the durable half of publication. The lock also
// serializes retirement and application of the single on-disk decision.
class LocalTransactionJournal {
  friend class Consumer;
  friend class Producer;
  friend class Destination;
  friend class Exchange;
  friend class Session;
  friend class linear_storage::Storage;

  using Storage = linear_storage::ConcurrentLinearStorage;

  struct Address {
    std::string path;
    Poco::UUID store;
  };

  struct Operation {
    Address address;
    Poco::UUID key;
    std::vector<char> bytes;  // empty for remove
    Storage* storage = nullptr;
    linear_storage::Record applied;
  };

  struct DeliveryAttempt {
    enum Phase : uint8_t { inFlight = 1, scheduled = 2, terminal = 3 };
    Phase phase = inFlight;
    int32_t count = 0;
    int64_t nextRedeliveryAt = 0;
    int32_t limit = -1;
    int64_t backoff = 0;
    int64_t maxBackoff = 0;
    std::optional<Address> deadLetter;
  };

  explicit LocalTransactionJournal(const Poco::Path& normalizedRoot);
  ~LocalTransactionJournal() noexcept;
  bool openDecision(bool create);
  void writeDecisionBytes(const void* bytes, size_t size, uint64_t offset);
  void resizeDecision(uint64_t size);
  void retireDecision();
  Address address(const Storage& storage) const;
  std::filesystem::path resolve(const Address& address) const;
  void recover();
  void decide(std::vector<Operation>& operations, std::string_view context);
  void apply(std::vector<Operation>& operations, std::string_view context);
  static void point(std::string_view context, std::string_view name);

  std::optional<DeliveryAttempt> attempt(const Storage& storage, const Poco::UUID& key) const;
  bool markInlineAttempt(Storage& storage, const linear_storage::Record& record);
  void saveAttempt(Storage& storage, const Poco::UUID& key, const DeliveryAttempt& state);
  void sweepAttempts(Storage& storage);
  static void removeAttempt(const Poco::Path& directory, const Poco::UUID& key) noexcept;
  [[noreturn]] static void stopAfterDecision(const char* detail) noexcept;

  std::filesystem::path _root;
#ifdef _WIN32
  std::filesystem::path _decisionPath;
  std::fstream _decisionFile;
#else
  int openAttemptDirectory(const Poco::Path& storage, bool create = false) const;
  int _rootFd = -1;
  int _decisionFd = -1;
#endif
  uint64_t _decisionSize = 0;
  std::mutex _mutex;
};
}
#endif
