#include "LocalTransactionJournal.h"
#include "Destination.h"
#include <Poco/Checksum.h>
#include <Poco/Exception.h>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <span>
#ifndef _WIN32
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
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

std::vector<char> decodeFrame(std::span<const char> bytes, std::string_view header) {
  Decoder frame(bytes);
  frame.expect(header);
  const auto payload = frame.take(frame.number<uint64_t>());
  if (frame.number<uint32_t>() != checksum(payload)) invalid();
  frame.expect(trailer);
  frame.end();
  return {payload.begin(), payload.end()};
}

size_t frameSize(uint64_t size, uint64_t offset) {
  if (size < offset || size - offset < 28 || size - offset > static_cast<uint64_t>(UINT32_MAX) + 28 ||
      size - offset > SIZE_MAX) invalid();
  return static_cast<size_t>(size - offset);
}

#ifdef _WIN32
void writeFrame(const fs::path& path, std::string_view header, const Encoder& payload) {
  const auto frame = encodeFrame(header, payload);
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.exceptions(std::ios::badbit | std::ios::failbit);
  output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  output.close();
}

std::vector<char> readFrame(const fs::path& path, std::string_view header, uint64_t offset = 0) {
  const auto size = frameSize(fs::file_size(path), offset);
  std::ifstream input(path, std::ios::binary);
  input.exceptions(std::ios::badbit | std::ios::failbit);
  input.seekg(offset);
  std::vector<char> bytes(size);
  input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return decodeFrame(bytes, header);
}

fs::path attemptPath(const Poco::Path& directory, const Poco::UUID& key) {
  return fs::path(directory.toString()) / "attempt-72" / key.toString();
}
#else
class Descriptor {
 public:
  explicit Descriptor(int fd = -1) : _fd(fd) {}
  ~Descriptor() { if (_fd >= 0) ::close(_fd); }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int get() const { return _fd; }
  int release() { return std::exchange(_fd, -1); }
  void reset(int fd) {
    if (_fd >= 0) ::close(_fd);
    _fd = fd;
  }

 private:
  int _fd;
};

[[noreturn]] void unsafeEntry(const char* name) {
  throw Poco::DataFormatException("unsafe delivery metadata entry", name);
}

int openDirectory(int parent, const char* name, bool optional = false, bool create = false,
                  int access = O_RDONLY) {
  const int flags = access | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
  int fd = ::openat(parent, name, flags);
  if (fd < 0 && errno == ENOENT && create) {
    if (::mkdirat(parent, name, 0700) < 0 && errno != EEXIST) throw Poco::CreateFileException(name);
    fd = ::openat(parent, name, flags);
  }
  if (fd >= 0 || (optional && errno == ENOENT)) return fd;
  if (errno == ENOTDIR || errno == ELOOP) unsafeEntry(name);
  throw Poco::OpenFileException(name);
}

// Walk one component at a time so swapping any opened path for a link cannot
// redirect later operations. The caller owns the starting descriptor.
int openDirectoryPath(int parent, const fs::path& path, bool optional = false,
                      int ancestorAccess = O_RDONLY) {
  Descriptor directory;
  for (auto it = path.begin(); it != path.end(); ++it) {
    const auto& part = *it;
    if (part.empty() || part == ".") continue;
    if (part == ".." || part.has_root_path()) unsafeEntry(part.c_str());
    const int access = ancestorAccess == O_RDONLY || std::next(it) == path.end() ? O_RDONLY : ancestorAccess;
    directory.reset(openDirectory(parent, part.c_str(), optional, false, access));
    parent = directory.get();
    if (parent < 0) return -1;
  }
  return directory.release();
}

int openAbsoluteDirectory(const fs::path& path, bool optional = false) {
#ifdef O_SEARCH
  if (path == path.root_path()) return openDirectory(AT_FDCWD, "/");
  // Intermediate directories need search permission only. The final directory
  // is still opened for reading, including paths with a trailing separator.
  Descriptor root(openDirectory(AT_FDCWD, "/", false, false, O_SEARCH));
  const auto relative = (path.has_filename() ? path : path.parent_path()).relative_path();
  return openDirectoryPath(root.get(), relative, optional, O_SEARCH);
#else
  Descriptor root(openDirectory(AT_FDCWD, "/"));
  if (path == path.root_path()) return root.release();
  return openDirectoryPath(root.get(), path.relative_path(), optional);
#endif
}

int openFile(int directory, const char* name, int flags, bool optional = false) {
  // Nonblocking open lets fstat reject a FIFO even without a peer.
  const int fd = ::openat(directory, name, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
  if (fd >= 0 || (optional && errno == ENOENT)) return fd;
  if (errno == ELOOP || errno == EISDIR) unsafeEntry(name);
  throw Poco::OpenFileException(name);
}

uint64_t regularFileSize(int fd, const char* name) {
  struct stat info {};
  if (::fstat(fd, &info) < 0) throw Poco::FileException(name);
  if (!S_ISREG(info.st_mode)) unsafeEntry(name);
  return static_cast<uint64_t>(info.st_size);
}

void readBytes(int fd, char* bytes, size_t size, uint64_t offset) {
  while (size != 0) {
    const auto result = ::pread(fd, bytes, size, static_cast<off_t>(offset));
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) throw Poco::IOException("delivery-72 metadata read failed");
    bytes += result;
    size -= static_cast<size_t>(result);
    offset += static_cast<uint64_t>(result);
  }
}

std::vector<char> readFrame(int fd, uint64_t size, std::string_view header, uint64_t offset = 0) {
  std::vector<char> bytes(frameSize(size, offset));
  readBytes(fd, bytes.data(), bytes.size(), offset);
  return decodeFrame(bytes, header);
}

void unlinkEntry(int directory, const char* name) {
  if (::unlinkat(directory, name, 0) < 0 && errno != ENOENT) throw Poco::FileException(name);
}

void writeAttempt(int directory, const std::string& name, const Encoder& body) {
  const auto frame = encodeFrame(attemptMagic, body);
  const auto temporary = name + ".tmp";
  // A stale temporary entry can be unlinked even if it is a link. Exclusive
  // creation also rejects an entry substituted between unlink and open.
  unlinkEntry(directory, temporary.c_str());
  Descriptor file(openFile(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL));
  size_t offset = 0;
  while (offset < frame.size()) {
    const auto result = ::write(file.get(), frame.data() + offset, frame.size() - offset);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) throw Poco::IOException("delivery-72 attempt write failed");
    offset += static_cast<size_t>(result);
  }
  if (::renameat(directory, temporary.c_str(), directory, name.c_str()) < 0) {
    throw Poco::FileException(name);
  }
}
#endif

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

LocalTransactionJournal::LocalTransactionJournal(const Poco::Path& normalizedRoot)
    : _root(normalizedRoot.toString())
#ifdef _WIN32
      , _decisionPath(_root / "delivery-72" / "decision")
#endif
{}

LocalTransactionJournal::~LocalTransactionJournal() noexcept {
#ifndef _WIN32
  if (_decisionFd >= 0 && ::close(_decisionFd) < 0) std::fprintf(stderr, "delivery-72: cannot close journal\n");
  if (_rootFd >= 0) ::close(_rootFd);
#endif
}

bool LocalTransactionJournal::openDecision(bool create) {
#ifdef _WIN32
  if (_decisionFile.is_open()) return true;
  if (!create && !fs::exists(_decisionPath)) return false;
  if (create) fs::create_directories(_decisionPath.parent_path());
  if (!fs::exists(_decisionPath)) std::ofstream(_decisionPath, std::ios::binary).close();
  _decisionFile.open(_decisionPath, std::ios::binary | std::ios::in | std::ios::out);
  if (!_decisionFile) throw Poco::OpenFileException(_decisionPath.string());
  _decisionSize = fs::file_size(_decisionPath);
#else
  if (_decisionFd >= 0) return true;
  Descriptor directory(openDirectory(_rootFd, "delivery-72", !create, create));
  if (directory.get() < 0) return false;
  Descriptor file(openFile(directory.get(), "decision", O_RDWR | (create ? O_CREAT : 0), !create));
  if (file.get() < 0) return false;
  _decisionSize = regularFileSize(file.get(), "decision");
  _decisionFd = file.release();
#endif
  return true;
}

#ifndef _WIN32
int LocalTransactionJournal::openAttemptDirectory(const Poco::Path& storage, bool create) const {
  const auto path = fs::absolute(storage.toString()).lexically_normal();
  const auto relative = path.lexically_relative(_root);
  if (relative.empty() || relative == ".") invalid();
  Descriptor parent(openDirectoryPath(_rootFd, relative, !create));
  if (parent.get() < 0) return -1;
  return openDirectory(parent.get(), "attempt-72", !create, create);
}
#endif

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
#ifdef _WIN32
  if (!fs::exists(_decisionPath) || fs::file_size(_decisionPath) == 0) return;
  std::ifstream input(_decisionPath, std::ios::binary);
  const auto state = input.get();
#else
  _rootFd = openAbsoluteDirectory(_root);
  if (!openDecision(false) || _decisionSize == 0) return;
  char state;
  readBytes(_decisionFd, &state, 1, 0);
#endif
  if (state == 0) return;
  if (state != 1) invalid();
#ifdef _WIN32
  const auto bytes = readFrame(_decisionPath, magic, 1);
#else
  const auto bytes = readFrame(_decisionFd, _decisionSize, magic, 1);
#endif
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
#ifdef _WIN32
  openDecision(true);
#endif
  retireDecision();
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
  openDecision(true);
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
#ifdef _WIN32
  const auto path = attemptPath(storage._linearStorage._basePath, key);
  if (!fs::exists(path)) return {};
  const auto bytes = readFrame(path, attemptMagic);
#else
  Descriptor directory(openAttemptDirectory(storage._linearStorage._basePath));
  if (directory.get() < 0) return {};
  const auto name = key.toString();
  Descriptor file(openFile(directory.get(), name.c_str(), O_RDONLY, true));
  if (file.get() < 0) return {};
  const auto bytes = readFrame(file.get(), regularFileSize(file.get(), name.c_str()), attemptMagic);
#endif
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
#ifdef _WIN32
  const auto path = attemptPath(storage._linearStorage._basePath, key);
  if (!storage._linearStorage._hasAttemptFiles) fs::create_directory(path.parent_path());
  auto temporary = path;
  temporary += ".tmp";
  writeFrame(temporary, attemptMagic, body);
  fs::rename(temporary, path);
#else
  Descriptor directory(openAttemptDirectory(storage._linearStorage._basePath, true));
  writeAttempt(directory.get(), key.toString(), body);
#endif
  storage._linearStorage._hasAttemptFiles = true;
}

void LocalTransactionJournal::sweepAttempts(Storage& storage) {
  std::lock_guard lock(storage._storageMutex);
  auto& linear = storage._linearStorage;
#ifdef _WIN32
  const auto directory = fs::path(linear._basePath.toString()) / "attempt-72";
  if (!fs::exists(directory)) return;
  for (const auto& entry : fs::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (entry.path().extension() == ".tmp" ||
        linear.record(Poco::UUID(name)).tomId == missing) {
      fs::remove(entry.path());
    }
  }
#else
  Descriptor directory(openAttemptDirectory(linear._basePath));
  if (directory.get() < 0) return;
  const auto closeDirectory = [](DIR* stream) { ::closedir(stream); };
  std::unique_ptr<DIR, decltype(closeDirectory)> entries(::fdopendir(directory.get()), closeDirectory);
  if (!entries) throw Poco::OpenFileException("attempt-72");
  const int fd = directory.release();  // owned by entries
  for (;;) {
    errno = 0;
    const auto* entry = ::readdir(entries.get());
    if (!entry) {
      if (errno != 0) throw Poco::FileException("attempt-72");
      break;
    }
    const std::string name(entry->d_name);
    if (name == "." || name == "..") continue;
    if (name.ends_with(".tmp") || linear.record(Poco::UUID(name)).tomId == missing) {
      unlinkEntry(fd, name.c_str());
    }
  }
#endif
}

void LocalTransactionJournal::removeAttempt(const Poco::Path& directory, const Poco::UUID& key) noexcept {
  try {
#ifdef _WIN32
    fs::remove(attemptPath(directory, key));
#else
    Descriptor storage(openAbsoluteDirectory(fs::absolute(directory.toString()).lexically_normal(), true));
    if (storage.get() < 0) return;
    Descriptor attempts(openDirectory(storage.get(), "attempt-72", true));
    if (attempts.get() < 0) return;
    unlinkEntry(attempts.get(), key.toString().c_str());
#endif
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
