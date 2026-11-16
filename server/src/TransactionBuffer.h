#ifndef TINY_MQ__TRANSACTION_BUFFER_H_
#define TINY_MQ__TRANSACTION_BUFFER_H_

#include <Poco/Path.h>
#include <Poco/UUID.h>
#include <Poco/Logger.h>
#include <Poco/Mutex.h>
#include <Poco/File.h>
#include <Poco/FileStream.h>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <fstream>
#include "Message.h"
#include "ConcurrentLinearStorage.h"
#include "HashHelper.h"

namespace tiny_mq {
class TransactionBuffer {
public:
    using Ptr = std::shared_ptr<TransactionBuffer>;

    TransactionBuffer(const Poco::Path& basePath,
                     std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage);
    ~TransactionBuffer();

    // Transaction lifecycle
    void addMessage(const std::string& transactionId, const Poco::UUID& messageId,
                   const std::vector<char>& data);
    // Patch the 8-byte deliveryTime field (offset 43, 0x02 wire format) in an
    // already-buffered-but-not-yet-committed record. Used by Producer::commit()
    // to resolve a transactional send's delay clock, which starts at commit —
    // not send — per JMS 2.0 §7.8 (spec 13). No-op if messageId has no buffered
    // data (e.g. not persistent) or the record predates the 0x02 format.
    void patchDeliveryTime(const Poco::UUID& messageId, int64_t deliveryTime);
    void commitTransaction(const std::string& transactionId);
    void rollbackTransaction(const std::string& transactionId);
    // Recovery
    void recover();
private:
    Poco::Path _basePath;
    std::reference_wrapper<Poco::Logger> _logger;
    std::shared_ptr<linear_storage::ConcurrentLinearStorage> _storage;

    mutable Poco::FastMutex _mutex;

    // In-memory transaction state
    std::unordered_map<std::string, std::vector<Poco::UUID>> _transactionMessages;
    std::unordered_map<Poco::UUID, std::string> _messageToTransaction;
    std::unordered_map<Poco::UUID, std::vector<char>> _bufferedData;

    // Persistent transaction log
    Poco::Path _transactionLogPath;
    std::ofstream _transactionLog;

    // Helper methods (call these only while holding _mutex)
    void ensureLogOpenLocked();
    void logOperationLocked(const std::string& operation, const std::string& transactionId,
                            const Poco::UUID& messageId = Poco::UUID());
    void loadTransactionLog();
    std::vector<std::string> getIncompleteTransactionsLocked() const;
};

} // namespace tiny_mq

#endif // TINY_MQ__TRANSACTION_BUFFER_H_