//
// Created for transaction buffering in tiny-mq
//

#include "TransactionBuffer.h"
#include "LogTracer.h"
#include <Poco/FileStream.h>
#include <Poco/StringTokenizer.h>
#include <Poco/DateTimeFormatter.h>
#include <sstream>
#include <fstream>

namespace tiny_mq {

TransactionBuffer::TransactionBuffer(const Poco::Path& basePath,
                                   std::shared_ptr<linear_storage::ConcurrentLinearStorage> storage)
    : _logger(Poco::Logger::get("tiny_mq.transaction_buffer")),
      _storage(storage) {
    TRACE(_logger);

    _basePath = Poco::Path(basePath).append("transactions");
    Poco::File dir(_basePath);
    dir.createDirectories();

    _transactionLogPath = Poco::Path(_basePath).append("transaction.log");

    loadTransactionLog();
}

TransactionBuffer::~TransactionBuffer() {
    TRACE(_logger);
    if (_transactionLog.is_open()) {
        _transactionLog.close();
    }
}

// Must be called with _mutex already held
void TransactionBuffer::ensureLogOpenLocked() {
    if (!_transactionLog.is_open()) {
        _transactionLog.open(_transactionLogPath.toString(), std::ios::app | std::ios::out);
        if (!_transactionLog) {
            throw Poco::OpenFileException("Cannot open transaction log: " + _transactionLogPath.toString());
        }
    }
}

// Must be called with _mutex already held
void TransactionBuffer::logOperationLocked(const std::string& operation,
                                           const std::string& transactionId,
                                           const Poco::UUID& messageId) {
    ensureLogOpenLocked();
    _transactionLog << Poco::DateTimeFormatter::format(Poco::Timestamp(), "%Y-%m-%d %H:%M:%S.%i")
                    << "|" << operation
                    << "|" << transactionId
                    << "|" << messageId.toString()
                    << "\n";
    _transactionLog.flush();
}

void TransactionBuffer::addMessage(const std::string& transactionId,
                                  const Poco::UUID& messageId,
                                  const std::vector<char>& data) {
    TRACE(_logger);
    Poco::ScopedLock<Poco::FastMutex> lock(_mutex);

    // Auto-begin if transaction doesn't exist yet
    auto it = _transactionMessages.find(transactionId);
    if (it == _transactionMessages.end()) {
        _transactionMessages[transactionId] = {};
        logOperationLocked("BEGIN", transactionId);
        it = _transactionMessages.find(transactionId);
    }

    _bufferedData[messageId] = data;
    _messageToTransaction[messageId] = transactionId;
    it->second.push_back(messageId);

    logOperationLocked("ADD", transactionId, messageId);
    poco_debug(_logger.get(), Poco::format("Added message %s to transaction %s",
                                          messageId.toString(), transactionId));
}


void TransactionBuffer::commitTransaction(const std::string& transactionId) {
    TRACE(_logger);
    std::vector<std::pair<Poco::UUID, std::vector<char>>> batch;

    {
        Poco::ScopedLock<Poco::FastMutex> lock(_mutex);

        auto it = _transactionMessages.find(transactionId);
        if (it == _transactionMessages.end()) {
            poco_debug(_logger.get(), Poco::format("commitTransaction: no active transaction %s (already committed or never begun)", transactionId));
            return;
        }

        batch.reserve(it->second.size());
        for (const auto& messageId : it->second) {
            auto dataIt = _bufferedData.find(messageId);
            if (dataIt != _bufferedData.end()) {
                batch.emplace_back(messageId, std::move(dataIt->second));
                _bufferedData.erase(dataIt);
                _messageToTransaction.erase(messageId);
            }
        }

        _transactionMessages.erase(it);
        logOperationLocked("COMMIT", transactionId);
        poco_debug(_logger.get(), Poco::format("Committed transaction %s", transactionId));
    }  // release lock before storage I/O

    if (_storage && !batch.empty()) {
        _storage->appendBatch(std::move(batch));
    }
}

void TransactionBuffer::rollbackTransaction(const std::string& transactionId) {
    TRACE(_logger);
    Poco::ScopedLock<Poco::FastMutex> lock(_mutex);

    auto it = _transactionMessages.find(transactionId);
    if (it == _transactionMessages.end()) {
        poco_debug(_logger.get(), Poco::format("rollbackTransaction: no active transaction %s", transactionId));
        return;
    }

    for (const auto& messageId : it->second) {
        _bufferedData.erase(messageId);
        _messageToTransaction.erase(messageId);
    }

    _transactionMessages.erase(it);
    logOperationLocked("ROLLBACK", transactionId);
    poco_debug(_logger.get(), Poco::format("Rolled back transaction %s", transactionId));
}

void TransactionBuffer::loadTransactionLog() {
    TRACE(_logger);
    Poco::ScopedLock<Poco::FastMutex> lock(_mutex);

    _transactionMessages.clear();
    _messageToTransaction.clear();
    _bufferedData.clear();

    std::ifstream logFile(_transactionLogPath.toString());
    if (!logFile) {
        poco_information(_logger.get(), "No existing transaction log found");
        return;
    }

    std::string line;
    while (std::getline(logFile, line)) {
        if (line.empty()) continue;

        Poco::StringTokenizer tok(line, "|", Poco::StringTokenizer::TOK_TRIM);
        if (tok.count() < 3) continue;

        const std::string& operation = tok[1];
        const std::string& transactionId = tok[2];

        if (operation == "BEGIN") {
            _transactionMessages[transactionId] = {};
        } else if (operation == "ADD" && tok.count() >= 4) {
            Poco::UUID messageId;
            try { messageId.parse(tok[3]); } catch (...) { continue; }
            auto it = _transactionMessages.find(transactionId);
            if (it != _transactionMessages.end()) {
                it->second.push_back(messageId);
                _messageToTransaction[messageId] = transactionId;
            }
        } else if (operation == "COMMIT" || operation == "ROLLBACK") {
            _transactionMessages.erase(transactionId);
        }
    }

    poco_information(_logger.get(), Poco::format("Loaded %z transactions from log",
                                                _transactionMessages.size()));
}

std::vector<std::string> TransactionBuffer::getIncompleteTransactionsLocked() const {
    std::vector<std::string> incomplete;
    for (const auto& [id, _] : _transactionMessages) {
        incomplete.push_back(id);
    }
    return incomplete;
}

void TransactionBuffer::recover() {
    TRACE(_logger);
    Poco::ScopedLock<Poco::FastMutex> lock(_mutex);

    auto incomplete = getIncompleteTransactionsLocked();
    for (const auto& transactionId : incomplete) {
        poco_warning(_logger.get(), Poco::format("Rolling back incomplete transaction %s", transactionId));

        // Clean up in-memory state (no mutex re-entry)
        auto it = _transactionMessages.find(transactionId);
        if (it != _transactionMessages.end()) {
            for (const auto& msgId : it->second) {
                _bufferedData.erase(msgId);
                _messageToTransaction.erase(msgId);
            }
            _transactionMessages.erase(it);
        }
        logOperationLocked("ROLLBACK", transactionId);
    }

    poco_information(_logger.get(), Poco::format("Recovered %z incomplete transactions",
                                                incomplete.size()));
}


} // namespace tiny_mq
