#include "LocalTransactionJournal.h"
#include "Destination.h"
#include <Poco/Checksum.h>
#include <Poco/Exception.h>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <span>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace tiny_mq {
namespace {
namespace fs = std::filesystem;
constexpr std::string_view magic = "TMQ72W01";
constexpr std::string_view attemptMagic = "TMQ72A01";
constexpr std::string_view trailer = "TMQ72END";

[[noreturn]] void invalid() { throw Poco::DataFormatException("invalid delivery-72 frame"); }

class Encoder {
 public:
  template<class T> void number(T value) {
    auto raw = std::bit_cast<std::make_unsigned_t<T>>(value);
    for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(static_cast<char>(raw >> (8 * i)));
  }
  void append(std::span<const char> values) { bytes.insert(bytes.end(), values.begin(), values.end()); }
  void uuid(const Poco::UUID& value) {
    std::array<char, 16> raw;
    value.copyTo(raw.data());
    append(raw);
  }
  void string(std::string_view value) {
    if (value.size() > UINT32_MAX) throw Poco::RangeException("delivery-72 path too long");
    number(static_cast<uint32_t>(value.size()));
    append(value);
  }

  std::vector<char> bytes;
};

class Decoder {
 public:
  explicit Decoder(std::span<const char> bytes) : _bytes(bytes) {}
  std::span<const char> take(uint64_t size) {
    if (size > _bytes.size()) invalid();
    const auto values = _bytes.first(static_cast<size_t>(size));
    _bytes = _bytes.subspan(static_cast<size_t>(size));
    return values;
  }
  template<class T> T number() {
    auto values = take(sizeof(T));
    std::make_unsigned_t<T> raw = 0;
    for (size_t i = 0; i < sizeof(T); ++i) {
      raw |= static_cast<std::make_unsigned_t<T>>(static_cast<unsigned char>(values[i])) << (8 * i);
    }
    return std::bit_cast<T>(raw);
  }
  Poco::UUID uuid() {
    Poco::UUID value;
    value.copyFrom(take(16).data());
    return value;
  }
  std::string string() {
    const auto values = take(number<uint32_t>());
    return {values.begin(), values.end()};
  }
  void expect(std::string_view value) {
    const auto actual = take(value.size());
    if (!std::equal(actual.begin(), actual.end(), value.begin())) invalid();
  }
  void end() const { if (!_bytes.empty()) invalid(); }

 private:
  std::span<const char> _bytes;
};

uint32_t checksum(std::span<const char> bytes) {
  Poco::Checksum crc(Poco::Checksum::TYPE_CRC32);
  crc.update(bytes.data(), static_cast<unsigned>(bytes.size()));
  return crc.checksum();
}

std::vector<char> encodeFrame(std::string_view header, const Encoder& payload) {
  if (payload.bytes.size() > UINT32_MAX) throw Poco::RangeException("delivery-72 frame too large");
  Encoder frame;
  frame.bytes.reserve(payload.bytes.size() + 28);
  frame.append(header);
  frame.number(static_cast<uint64_t>(payload.bytes.size()));
  frame.append(payload.bytes);
  frame.number(checksum(payload.bytes));
  frame.append(trailer);
  return std::move(frame.bytes);
}

void writeFrame(const fs::path& path, std::string_view header, const Encoder& payload) {
  const auto frame = encodeFrame(header, payload);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  output.close();
}

std::vector<char> readFrame(const fs::path& path, std::string_view header, uint64_t offset = 0) {
  const auto size = fs::file_size(path) - offset;
  if (size > static_cast<uint64_t>(UINT32_MAX) + 28 || size < 28) invalid();
  std::ifstream input(path, std::ios::binary);
  input.exceptions(std::ios::badbit | std::ios::failbit);
  input.seekg(offset);
  std::vector<char> bytes(static_cast<size_t>(size));
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  Decoder frame(bytes);
  frame.expect(header);
  const auto payload = frame.take(frame.number<uint64_t>());
  if (frame.number<uint32_t>() != checksum(payload)) invalid();
  frame.expect(trailer);
  frame.end();
  return {payload.begin(), payload.end()};
}

fs::path attemptPath(const Poco::Path& directory, const Poco::UUID& key) {
  return fs::path(directory.toString()) / "attempt-72" / key.toString();
}

constexpr auto missing = std::numeric_limits<Poco::UInt32>::max();
#ifdef ENABLE_TESTS
std::atomic<const std::function<void()>*> beforeCommit{nullptr};
#endif
}

#ifdef ENABLE_TESTS
namespace internal {
void observeCommitPreparation(const std::function<void()>* observer) { beforeCommit = observer; }
}
#endif

LocalTransactionJournal::LocalTransactionJournal(const Poco::Path& root)
    : _root(fs::weakly_canonical(fs::absolute(root.toString()))), _directory(_root / "delivery-72"),
      _decisionPath(_directory / "decision") {}

LocalTransactionJournal::~LocalTransactionJournal() noexcept {
#ifndef _WIN32
  if (_decisionFd >= 0 && ::close(_decisionFd) < 0) std::fprintf(stderr, "delivery-72: cannot close journal\n");
#endif
}

void LocalTransactionJournal::openDecision() {
#ifdef _WIN32
  if (_decisionFile.is_open()) return;
#else
  if (_decisionFd >= 0) return;
#endif
  fs::create_directories(_directory);
#ifdef _WIN32
  if (!fs::exists(_decisionPath)) std::ofstream(_decisionPath, std::ios::binary).close();
  _decisionFile.open(_decisionPath, std::ios::binary | std::ios::in | std::ios::out);
  if (!_decisionFile) throw Poco::OpenFileException(_decisionPath.string());
#else
  _decisionFd = ::open(_decisionPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (_decisionFd < 0) throw Poco::OpenFileException(_decisionPath.string());
#endif
  _decisionSize = fs::file_size(_decisionPath);
}

void LocalTransactionJournal::writeDecisionBytes(const void* bytes, size_t size, uint64_t offset) {
#ifdef _WIN32
  _decisionFile.clear();
  _decisionFile.seekp(offset);
  _decisionFile.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
  _decisionFile.flush();
  const bool complete = _decisionFile.good();
#else
  ssize_t result;
  do {
    result = ::pwrite(_decisionFd, bytes, size, static_cast<off_t>(offset));
  } while (result < 0 && errno == EINTR);
  const bool complete = result >= 0 && static_cast<size_t>(result) == size;
#endif
  if (!complete) throw Poco::IOException("delivery-72 journal write failed");
}

void LocalTransactionJournal::resizeDecision(uint64_t size) {
#ifdef _WIN32
  fs::resize_file(_decisionPath, size);
#else
  if (::ftruncate(_decisionFd, static_cast<off_t>(size))) throw Poco::IOException("delivery-72 journal truncate failed");
#endif
  _decisionSize = size;
}

void LocalTransactionJournal::retireDecision() {
  constexpr char retired = 0;
  writeDecisionBytes(&retired, 1, 0);
  if (_decisionSize > 1024 * 1024) resizeDecision(1);
}

LocalTransactionJournal::Address LocalTransactionJournal::address(const Storage& storage) const {
  const auto& linear = storage._linearStorage;
  return {linear._directory.lexically_relative(_root).generic_string(), linear._id};
}

fs::path LocalTransactionJournal::resolve(const Address& address) const {
  const fs::path relative(address.path);
  if (relative.empty() || relative.is_absolute()) invalid();
  for (const auto& part : relative) {
    if (part == ".." || part == "." || part.empty()) invalid();
  }
  const auto absolute = _root / relative;
  if (fs::weakly_canonical(absolute) != absolute) invalid();
  return absolute;
}

void LocalTransactionJournal::recover() {
  if (fs::exists(_decisionPath) && fs::file_size(_decisionPath) != 0) {
    std::ifstream input(_decisionPath, std::ios::binary);
    const auto state = input.get();
    if (state == 0) return;
    if (state != 1) invalid();
    const auto bytes = readFrame(_decisionPath, magic, 1);
    Decoder payload(bytes);
    const auto count = payload.number<uint32_t>();
    if (count == 0 || count > bytes.size() / 37) invalid();
    std::vector<Operation> operations;
    operations.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const auto tag = payload.number<uint8_t>();
      Operation operation{{payload.string(), payload.uuid()}, payload.uuid(), {}, nullptr, {}};
      resolve(operation.address);
      if (operation.address.store.isNull() || operation.key.isNull()) invalid();
      if (tag == 1) {
        const auto data = payload.take(payload.number<uint64_t>());
        if (data.empty()) invalid();
        operation.bytes.assign(data.begin(), data.end());
        Destination::decodeStored(operation.bytes);
      } else if (tag != 2) {
        invalid();
      }
      operations.push_back(std::move(operation));
    }
    payload.end();
    apply(operations, "replay");
    openDecision();
    retireDecision();
  }
}

void LocalTransactionJournal::decide(std::vector<Operation>& operations, std::string_view context) {
  if (operations.empty()) return;
  std::lock_guard lock(_mutex);
  Encoder payload;
  if (operations.size() > UINT32_MAX) throw Poco::RangeException("too many delivery-72 operations");
  payload.number(static_cast<uint32_t>(operations.size()));
  for (const auto& operation : operations) {
    payload.number<uint8_t>(operation.bytes.empty() ? 2 : 1);
    payload.string(operation.address.path);
    payload.uuid(operation.address.store);
    payload.uuid(operation.key);
    if (!operation.bytes.empty()) {
      payload.number(static_cast<uint64_t>(operation.bytes.size()));
      payload.append(operation.bytes);
    }
  }
  const auto frame = encodeFrame(magic, payload);
  openDecision();
  // On the first write the leading hole reads as state 0 on process restart.
  writeDecisionBytes(frame.data(), frame.size(), 1);
  if (_decisionSize != frame.size() + 1) resizeDecision(frame.size() + 1);
  point(context, "before-decision");
#ifdef ENABLE_TESTS
  if (context == "commit") {
    if (auto observer = beforeCommit.load(std::memory_order_acquire)) (*observer)();
  }
  const auto* injected = std::getenv("TINY_MQ_TEST_COMMIT_ERROR");
  if (context == "commit" && injected && *injected) {
    throw Poco::IOException("injected failure before commit decision");
  }
#endif
  constexpr char committed = 1;
  writeDecisionBytes(&committed, 1, 0);
  point(context, "after-decision");
  try {
    apply(operations, context);
    retireDecision();
  } catch (const std::exception& error) {
    stopAfterDecision(error.what());
  } catch (...) {
    stopAfterDecision("unknown exception applying decision");
  }
}

void LocalTransactionJournal::apply(std::vector<Operation>& operations, std::string_view context) {
  using Linear = linear_storage::Storage;
  std::map<std::string, std::unique_ptr<Linear>> stores;
  for (size_t i = 0; i < operations.size(); ++i) {
    auto& operation = operations[i];
    if (operation.storage) {
      auto& concurrent = *operation.storage;
      std::lock_guard lock(concurrent._storageMutex);
      auto& storage = concurrent._linearStorage;
      if (!operation.bytes.empty()) {
        operation.applied = storage.append(operation.key, operation.bytes);
      } else {
        auto prior = operation.applied.tomId == missing ? storage.record(operation.key) : operation.applied;
        if (prior.tomId != missing && !storage.remove(prior)) throw Poco::IOException("delivery-72 remove failed");
        if (prior.tomId == missing && storage._hasAttemptFiles) removeAttempt(storage._basePath, operation.key);
      }
    } else {
      auto& entry = stores[operation.address.path];
      if (!entry) entry = std::make_unique<Linear>(operation.address.store,
                                                   Poco::Path(resolve(operation.address).string()).makeDirectory());
      if (entry->_id != operation.address.store) invalid();
      auto prior = entry->record(operation.key);
      if (!operation.bytes.empty()) {
        if (prior.tomId == missing) entry->append(operation.key, operation.bytes);
        else if (entry->data(prior) != operation.bytes) invalid();
      } else {
        if (prior.tomId != missing && !entry->remove(prior)) throw Poco::IOException("delivery-72 replay remove failed");
        if (prior.tomId == missing) removeAttempt(entry->_basePath, operation.key);
      }
    }
    if (i == 0) point(context, "apply-1");
  }
}

std::optional<LocalTransactionJournal::DeliveryAttempt> LocalTransactionJournal::attempt(
    const Storage& storage, const Poco::UUID& key) const {
  if (!storage._linearStorage._hasAttemptFiles) return {};
  const auto path = attemptPath(storage._linearStorage._basePath, key);
  if (!fs::exists(path)) return {};
  const auto bytes = readFrame(path, attemptMagic);
  Decoder body(bytes);
  DeliveryAttempt attempt;
  const auto phase = body.number<uint8_t>();
  if (phase < DeliveryAttempt::inFlight || phase > DeliveryAttempt::terminal) invalid();
  attempt.phase = static_cast<DeliveryAttempt::Phase>(phase);
  attempt.count = body.number<int32_t>();
  attempt.nextRedeliveryAt = body.number<int64_t>();
  attempt.limit = body.number<int32_t>();
  attempt.backoff = body.number<int64_t>();
  attempt.maxBackoff = body.number<int64_t>();
  const auto dlq = body.number<uint8_t>();
  if (dlq > 1 || attempt.count < 0 || attempt.nextRedeliveryAt < 0 ||
      attempt.backoff < 0 || attempt.maxBackoff < 0) invalid();
  if (dlq) {
    attempt.deadLetter = Address{body.string(), body.uuid()};
    resolve(*attempt.deadLetter);
  }
  body.end();
  return attempt;
}

bool LocalTransactionJournal::markInlineAttempt(Storage& storage, const linear_storage::Record& record) {
  std::lock_guard lock(storage._storageMutex);
  auto& volume = storage._linearStorage.tom(record.tomId);
  if (!volume._deliveryVolume) return false;
  volume.markAttempt(record.offset);
  return true;
}

void LocalTransactionJournal::saveAttempt(Storage& storage, const Poco::UUID& key,
                                          const DeliveryAttempt& state) {
  std::lock_guard lock(storage._storageMutex);
  if (storage._linearStorage.record(key).tomId == missing) return;  // TTL sweep won the race
  const auto path = attemptPath(storage._linearStorage._basePath, key);
  if (!storage._linearStorage._hasAttemptFiles) fs::create_directory(path.parent_path());
  auto temporary = path;
  temporary += ".tmp";
  Encoder body;
  body.number<uint8_t>(state.phase);
  body.number(state.count);
  body.number(state.nextRedeliveryAt);
  body.number(state.limit);
  body.number(state.backoff);
  body.number(state.maxBackoff);
  body.number<uint8_t>(state.deadLetter ? 1 : 0);
  if (state.deadLetter) {
    body.string(state.deadLetter->path);
    body.uuid(state.deadLetter->store);
  }
  writeFrame(temporary, attemptMagic, body);
  fs::rename(temporary, path);
  storage._linearStorage._hasAttemptFiles = true;
}

void LocalTransactionJournal::sweepAttempts(Storage& storage) {
  std::lock_guard lock(storage._storageMutex);
  auto& linear = storage._linearStorage;
  const auto directory = fs::path(linear._basePath.toString()) / "attempt-72";
  if (!fs::exists(directory)) return;
  for (const auto& entry : fs::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (entry.path().extension() == ".tmp" ||
        linear.record(Poco::UUID(name)).tomId == missing) {
      fs::remove(entry.path());
    }
  }
}

void LocalTransactionJournal::removeAttempt(const Poco::Path& directory, const Poco::UUID& key) noexcept {
  try {
    fs::remove(attemptPath(directory, key));
  } catch (const std::exception& error) {
    std::fprintf(stderr, "delivery-72 attempt cleanup: %s\n", error.what());
  }
}

void LocalTransactionJournal::point(std::string_view context, std::string_view name) {
#ifdef ENABLE_TESTS
  if (const auto* value = std::getenv("TINY_MQ_TEST_CRASH")) {
    const std::string_view configured(value);
    if (configured.size() == context.size() + 1 + name.size() &&
        configured.starts_with(context) && configured[context.size()] == ':' && configured.ends_with(name)) {
      std::_Exit(72);
    }
  }
#else
  (void)context;
  (void)name;
#endif
}

[[noreturn]] void LocalTransactionJournal::stopAfterDecision(const char* detail) noexcept {
  std::fprintf(stderr, "delivery-72: restart required after decision: %s\n", detail);
  std::_Exit(73);
}
}
