//
// Created by Alexander Bychuk on 16.09.2023.
//

#include "LinearStorage.h"
#include "LocalTransactionJournal.h"
#include <Poco/File.h>
#include <Poco/Format.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <utility>
#include <stdexcept>
#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace linear_storage {

static_assert(sizeof(Header) == 32 && offsetof(Header, deleted) == 24 && offsetof(Header, attempt) == 25);

Tom::Tom(Poco::UInt32 id, Poco::Path basePath) : _id(id), _basePath(std::move(basePath)) {
  const auto directory = std::filesystem::path(_basePath.toString());
  std::filesystem::create_directories(directory);
  const auto path = directory / (std::to_string(_id) + ".tom");
  const auto marker = directory / (std::to_string(_id) + ".delivery-72");
  if (std::filesystem::exists(path)) {
    _size = std::filesystem::file_size(path);
    _deliveryVolume = std::filesystem::exists(marker);
  } else {
    std::ofstream version(marker);
    version.exceptions(std::ios::badbit | std::ios::failbit);
    version.close();
    std::ofstream create(path, std::ios::binary);
    create.exceptions(std::ios::badbit | std::ios::failbit);
    create.close();
    _deliveryVolume = true;
  }
  open();
}

void Tom::open() {
  const auto path = Poco::Path(_basePath).append(std::to_string(_id)).setExtension("tom").toString();
#ifdef _WIN32
  _tomF = std::make_unique<Poco::FileStream>(path, std::ios::binary | std::ios::in | std::ios::out);
  if (!_tomF->good()) throw Poco::OpenFileException(path);
#else
  _fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (_fd < 0) throw Poco::OpenFileException(path);
#endif
}

void Tom::truncate(Poco::UInt64 size) {
#ifdef _WIN32
  _tomF.reset();
  const auto path = Poco::Path(_basePath).append(std::to_string(_id)).setExtension("tom").toString();
  std::filesystem::resize_file(path, size);
  open();
#else
  unmap();
  if (::ftruncate(_fd, static_cast<off_t>(size))) throw Poco::IOException("could not truncate incomplete storage record");
#endif
  _size = size;
}

Tom::Tom(Tom&& other) noexcept
    : _id(other._id), _basePath(std::move(other._basePath)), _size(other._size),
#ifdef _WIN32
      _tomF(std::move(other._tomF)),
#else
      _fd(std::exchange(other._fd, -1)), _mapped(std::exchange(other._mapped, nullptr)),
      _mappedSize(other._mappedSize), _mappedAt(other._mappedAt),
#endif
      _deliveryVolume(other._deliveryVolume) {}

Tom::~Tom() noexcept {
#ifndef _WIN32
  unmap();
  if (_fd >= 0 && ::close(_fd) < 0) std::fprintf(stderr, "storage: could not close volume\n");
#endif
}

Poco::UInt32 Tom::id() const { return _id; }

bool Tom::writeAt(const void* data, size_t size, Poco::UInt64 offset) {
#ifdef _WIN32
  _tomF->clear();
  _tomF->seekp(offset, std::ios::beg);
  _tomF->write(static_cast<const char*>(data), size);
  _tomF->flush();
  return _tomF->good();
#else
  ssize_t result;
  do {
    result = ::pwrite(_fd, data, size, static_cast<off_t>(offset));
  } while (result < 0 && errno == EINTR);
  return result >= 0 && static_cast<size_t>(result) == size;
#endif
}

bool Tom::readAt(void* data, size_t size, Poco::UInt64 offset) const {
#ifdef _WIN32
  _tomF->clear();
  _tomF->seekg(offset, std::ios::beg);
  _tomF->read(static_cast<char*>(data), size);
  return _tomF->good();
#else
  ssize_t result;
  do {
    result = ::pread(_fd, data, size, static_cast<off_t>(offset));
  } while (result < 0 && errno == EINTR);
  return result >= 0 && static_cast<size_t>(result) == size;
#endif
}

Record Tom::append(const Poco::UUID& uuid, const std::vector<char>& data) {
  if (data.empty()) throw Poco::InvalidArgumentException("Data cannot be empty");

  Record rec{_id, _size, {}};
  rec.header.dataSize = data.size();
  uuid.copyTo(rec.header.uuid.data());
  const size_t size = sizeof(rec.header) + data.size();
#ifdef _WIN32
  std::vector<char> bytes(size);
  std::memcpy(bytes.data(), &rec.header, sizeof(rec.header));
  std::memcpy(bytes.data() + sizeof(rec.header), data.data(), data.size());
  const bool written = writeAt(bytes.data(), bytes.size(), _size);
#else
  const iovec buffers[]{{&rec.header, sizeof(rec.header)}, {const_cast<char*>(data.data()), data.size()}};
  ssize_t result;
  do {
    result = ::pwritev(_fd, buffers, 2, static_cast<off_t>(_size));
  } while (result < 0 && errno == EINTR);
  const bool written = result >= 0 && static_cast<size_t>(result) == size;
#endif
  if (!written) {
    truncate(rec.offset);
    throw Poco::IOException("failed to write record to file");
  }
  _size += size;
  return rec;
}

Record Tom::record(Poco::UInt64 offset) const {
  if (offset >= _size) return {};
  Record rec{_id, offset, {}};
  if (!readAt(&rec.header, sizeof(rec.header), offset)) throw Poco::IOException("failed to read storage header");
  if (!_deliveryVolume) rec.header.attempt = {};
  return rec;
}

std::vector<char> Tom::data(const Record& record) const {
  std::vector<char> bytes(record.header.dataSize);
  if (!bytes.empty() && !readAt(bytes.data(), bytes.size(), record.offset + sizeof(Header))) {
    throw Poco::IOException("failed to read storage data");
  }
  return bytes;
}

std::vector<char> Tom::dataPrefix(const Record& record, size_t len) const {
  const auto size = std::min<Poco::UInt64>(len, record.header.dataSize);
  std::vector<char> bytes(size);
  if (size > 0 && !readAt(bytes.data(), bytes.size(), record.offset + sizeof(Header))) return {};
  return bytes;
}

#ifndef _WIN32
void Tom::unmap() noexcept {
  if (!_mapped) return;
  if (::munmap(_mapped, _mappedSize) < 0) std::fprintf(stderr, "storage: could not unmap volume\n");
  _mapped = nullptr;
}
#endif

bool Tom::setFlag(Poco::UInt64 offset) {
  if (offset >= _size) return false;
#ifndef _WIN32
  if (!_mapped || offset < _mappedAt || offset - _mappedAt >= _mappedSize) {
    unmap();
    static const size_t window = static_cast<size_t>(::getpagesize()) * 64;
    _mappedSize = window;
    _mappedAt = offset - offset % window;
    auto* mapped = ::mmap(nullptr, window, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, static_cast<off_t>(_mappedAt));
    if (mapped != MAP_FAILED) _mapped = static_cast<char*>(mapped);
  }
  if (_mapped) {
    _mapped[offset - _mappedAt] = 1;
    return true;
  }
#endif
  constexpr char flag = 1;
  return writeAt(&flag, sizeof(flag), offset);
}

bool Tom::remove(Record& record) {
  if (record.header.deleted) return true;
  const bool removed = setFlag(record.offset + offsetof(Header, deleted));
  if (removed) record.header.deleted = true;
  return removed;
}

void Tom::markAttempt(Poco::UInt64 offset) {
  if (!setFlag(offset + offsetof(Header, attempt))) throw Poco::IOException("failed to register delivery attempt");
}

Tom Tom::create(Poco::UInt32 id, Poco::Path basePath) { return {id, std::move(basePath)}; }

Storage::Storage(const Poco::UUID &id, Poco::Path basePath)
    : _id(id), _basePath(std::move(basePath)), _currrentTomId(0) {
  // Create the index path properly
  Poco::File fp(_basePath.makeDirectory());
  if (!fp.exists()) {
    fp.createDirectories();
  }
  _directory = std::filesystem::weakly_canonical(_basePath.toString());
  _hasAttemptFiles = std::filesystem::exists(_directory / "attempt-72");
  const auto indexPath = _directory / (_id.toString() + ".linear.storage.index");
  _index = std::make_unique<fc::DiskBTreeMap<MsgUUIDType, Record>>(indexPath, 1024 * 1024, false);

  // Pre-open any existing .tom files so records from a previous run are readable
  Poco::File dir(_basePath);
  if (dir.exists()) {
    std::vector<std::string> files;
    dir.list(files);
    for (const auto& f : files) {
      static const std::string ext = ".tom";
      if (f.size() > ext.size() && f.substr(f.size() - ext.size()) == ext) {
        const auto stem = f.substr(0, f.size() - ext.size());
        if (stem.find_first_not_of("0123456789") != std::string::npos) continue;
        const auto number = std::stoull(stem);
        if (number > std::numeric_limits<Poco::UInt32>::max() || std::to_string(number) != stem) continue;
        const auto tomId = static_cast<Poco::UInt32>(number);
        if (!_tom.contains(tomId)) {
          _tom.emplace(tomId, Tom(tomId, _basePath));
          if (tomId > _currrentTomId) _currrentTomId = tomId;
        }
      }
    }
  }

  // Rebuild the B-tree index from tom files — the MemoryResourceFixed allocator
  // reinitialises the entire pool on every open (destroying stored entries), so
  // we must repopulate the index from the raw tom data each time.
  rebuildIndex();
}

// Destructor implementation
Storage::~Storage() {
  // The _index will be destroyed automatically when Storage is destroyed
}

Tom &Storage::currentTom() {
  Poco::UInt32 ctd = _currrentTomId;
  if (auto current = _tom.find(ctd); current != _tom.end() && current->second._deliveryVolume) return current->second;
  ctd = ++_currrentTomId;
  auto it = _tom.emplace(ctd, Tom(ctd, _basePath));
  if (it.second) {
    return it.first->second;
  }
  throw Poco::Exception(Poco::format("Can't emplace new tom [%u]", ctd), -1);
}

const Tom &Storage::tom(Poco::UInt32 id) const { return _tom.at(id); }

Tom &Storage::tom(Poco::UInt32 id) { return _tom.at(id); }

Record Storage::append(const Poco::UUID &uuid, const std::vector<char> &data) {
  // Validate input
  if (data.empty()) {
    throw Poco::InvalidArgumentException("Data cannot be empty");
  }
  
  Tom &tom = currentTom();
  Record rec = tom.append(uuid, data);
  (*_index)[rec.header.uuid] = rec;
  return rec;
}

Record Storage::record(Poco::UInt32 tomId, Poco::UInt64 offset) const { 
  return tom(tomId).record(offset); 
}

Record Storage::record(const Poco::UUID &uuid) const {
  MsgUUIDType uuidRaw;
  uuid.copyTo(uuidRaw.data());

  auto it = _index->find(uuidRaw);
  if (it != _index->end()) {
    return it->second;
  }
  return {};
}

std::vector<char> Storage::data(const Record &record) const {
  const Tom &t = tom(record.tomId);
  return t.data(record);
}

bool Storage::remove(Record &record) {
  Tom &t = tom(record.tomId);
  // Recover the uuid so the in-memory index entry is dropped too. The fast ack
  // path passes a Record with only tomId/offset (empty header.uuid), so fall
  // back to reading the on-disk header. Without this the index keeps a stale
  // entry and record(uuid) would resolve a deleted record until restart.
  MsgUUIDType uuid = record.header.uuid;
  if (uuid == MsgUUIDType{}) {
    try {
      uuid = t.record(record.offset).header.uuid;
    } catch (...) {
      uuid = MsgUUIDType{};
    }
  }
  bool ok = t.remove(record);
  if (ok && uuid != MsgUUIDType{}) {
    _index->erase(uuid);
    record.header.uuid = uuid;
    if (_hasAttemptFiles) clearDeliveryAttempt(record);
  }
  return ok;
}

void Storage::clearDeliveryAttempt(const Record& record) const noexcept {
  Poco::UUID key;
  key.copyFrom(record.header.uuid.data());
  tiny_mq::LocalTransactionJournal::removeAttempt(_basePath, key);
}

// Rebuild the B-tree index by scanning all existing tom files.
// Called after opening existing toms so that record() lookups work correctly.
void Storage::rebuildIndex() {
  for (auto& [tomId, t] : _tom) {
    Poco::UInt64 offset = 0;
    while (offset < t._size) {
      const auto remaining = t._size - offset;
      if (remaining < sizeof(Header)) {
        if (tomId != _currrentTomId) throw Poco::DataFormatException("truncated non-final storage volume");
        t.truncate(offset);
        break;
      }
      auto rec = t.record(offset);
      if (rec.header.dataSize == 0 || rec.header.deleted < 0 || rec.header.deleted > 1) {
        throw Poco::DataFormatException("invalid storage record header");
      }
      if (rec.header.dataSize > remaining - sizeof(Header)) {
        if (tomId != _currrentTomId) throw Poco::DataFormatException("truncated non-final storage record");
        t.truncate(offset);
        break;
      }
      if (!rec.header.deleted) (*_index)[rec.header.uuid] = rec;
      offset += sizeof(Header) + rec.header.dataSize;
    }
  }
}

// Scan tom files directly — does NOT rely on the B-tree index (which is reset
// on every open by MemoryResourceFixed's constructor).
std::vector<std::pair<Record, std::vector<char>>> Storage::scan() const {
  std::vector<std::pair<Record, std::vector<char>>> result;
  for (const auto& [tomId, t] : _tom) {
    Poco::UInt64 offset = 0;
    while (true) {
      Record rec = t.record(offset);
      if (rec.tomId == std::numeric_limits<Poco::UInt32>::max()) break;
      if (rec.header.dataSize == 0) break;
      if (!rec.header.deleted) {
        try {
          auto data = t.data(rec);
          result.emplace_back(rec, std::move(data));
        } catch (...) {
          // Skip unreadable records
        }
      }
      offset += sizeof(Header) + rec.header.dataSize;
    }
  }
  return result;
}

std::vector<std::pair<Record, std::vector<char>>>
Storage::scanPrefix(size_t prefixLen, size_t maxRecords, SweepCursor &cursor) const {
  std::vector<std::pair<Record, std::vector<char>>> result;

  // Stable order over toms so the cursor is meaningful between calls.
  std::vector<Poco::UInt32> ids;
  ids.reserve(_tom.size());
  for (const auto &kv : _tom) ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());

  size_t scanned = 0;
  bool resuming = true;  // still skipping past toms that precede the cursor
  for (Poco::UInt32 id : ids) {
    if (resuming && id < cursor.tomId) continue;
    const Tom &t = tom(id);
    Poco::UInt64 offset = (resuming && id == cursor.tomId) ? cursor.offset : 0;
    resuming = false;
    while (true) {
      if (scanned >= maxRecords) {
        cursor = SweepCursor{id, offset};
        return result;
      }
      Record rec = t.record(offset);
      if (rec.tomId == std::numeric_limits<Poco::UInt32>::max()) break;
      if (rec.header.dataSize == 0) break;
      const Poco::UInt64 next = offset + sizeof(Header) + rec.header.dataSize;
      if (!rec.header.deleted) {
        auto prefix = t.dataPrefix(rec, prefixLen);
        if (!prefix.empty()) result.emplace_back(rec, std::move(prefix));
      }
      ++scanned;
      offset = next;
    }
  }
  // Reached the end of the store: reset for the next full pass.
  cursor = SweepCursor{};
  return result;
}

}  // namespace linear_storage
