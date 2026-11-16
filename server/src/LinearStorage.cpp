//
// Created by Alexander Bychuk on 16.09.2023.
//

#include "LinearStorage.h"
#include <Poco/File.h>
#include <Poco/Format.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace linear_storage {

Tom::Tom(Poco::UInt32 id, Poco::Path basePath) : _id(id), _basePath(std::move(basePath)) {
  Poco::Path path(_basePath);
  path.append(std::to_string(_id)).setExtension("tom");
  std::string pathStr = path.toString();

  // Ensure the containing directory exists
  Poco::File baseDir(_basePath.makeDirectory());
  if (!baseDir.exists()) {
    baseDir.createDirectories();
  }

  // Initialise _size from existing file, or create an empty file
  Poco::File f(pathStr);
  if (f.exists()) {
    _size = static_cast<Poco::UInt64>(f.getSize());
  } else {
    std::ofstream create(pathStr, std::ios::binary | std::ios::out);
    _size = 0;
  }

  _tomF = std::make_unique<Poco::FileStream>(pathStr, std::ios::binary | std::ios::in | std::ios::out);
}

Tom::Tom(Tom &&o) noexcept : _id(o._id), _basePath(std::move(o._basePath)), _size(o._size), _tomF(std::move(o._tomF)) {}

Tom::~Tom() noexcept {
};

Poco::UInt32 Tom::id() const { return _id; }

Record Tom::append(const Poco::UUID &uuid, const std::vector<char> &data) {
  // Validate input data
  if (data.empty()) {
    throw Poco::InvalidArgumentException("Data cannot be empty");
  }

  Record rec{};
  rec.tomId = _id;
  // Store data size in header only, not at the end of record
  size_t bufSize = sizeof(rec.header) + data.size();
  std::vector<char> buf(bufSize);
  {
    rec.offset = _size;
    _size += bufSize;
  }
  rec.header.dataSize = data.size();
  rec.header.deleted = 0;
  uuid.copyTo(rec.header.uuid.data());

  memcpy(&buf[0], (const char *)&rec.header, sizeof(rec.header));
  memcpy(&buf[sizeof(rec.header)], data.data(), data.size());

  _tomF->seekp(rec.offset, std::ios::beg);
  _tomF->write(buf.data(), buf.size());
  _tomF->flush();
  
  // Check for write errors
  if (!_tomF->good()) {
    throw Poco::RuntimeException("Failed to write record to file");
  }
  
  return rec;
}

Record Tom::record(Poco::UInt64 offset) const {
  Record rec{};
  if (offset < _size) {
    rec.tomId = _id;
    rec.offset = offset;
    _tomF->seekg(offset, std::ios::beg);
    _tomF->read((char *)&rec.header, sizeof(rec.header));
    
    // Check for read errors
    if (!_tomF->good()) {
      throw Poco::RuntimeException("Failed to read record header from file");
    }
  }
  return rec;
}

std::vector<char> Tom::data(const Record &record) const {
  if (record.header.dataSize == 0) {
    return {};
  }

  std::vector<char> data;
  data.resize(record.header.dataSize);
  auto dataOffset = record.offset + sizeof(record.header);
  _tomF->seekg(dataOffset, std::ios::beg);
  _tomF->read((char *)&data[0], record.header.dataSize);
  
  // Check for read errors
  if (!_tomF->good()) {
    throw Poco::RuntimeException("Failed to read record data from file");
  }

  return data;
}

std::vector<char> Tom::dataPrefix(const Record &record, size_t len) const {
  const size_t n = std::min<size_t>(len, static_cast<size_t>(record.header.dataSize));
  if (n == 0) {
    return {};
  }
  std::vector<char> out(n);
  _tomF->seekg(record.offset + sizeof(record.header), std::ios::beg);
  _tomF->read(out.data(), static_cast<std::streamsize>(n));
  if (!_tomF->good()) {
    return {};
  }
  return out;
}

bool Tom::remove(Record &record) {
  if (record.header.deleted) {
    return true;
  }

  // Set deleted flag at the position where it's stored in the header
  auto deleteOffset = record.offset + offsetof(Header, deleted);
  record.header.deleted = 1;
  _tomF->seekp(deleteOffset, std::ios::beg);
  _tomF->write((const char *)&record.header.deleted, sizeof(record.header.deleted));
  _tomF->flush();
  
  // Check for write errors
  if (!_tomF->good()) {
    record.header.deleted = 0;
    return false;
  }
  
  return true;
}

Tom Tom::create(Poco::UInt32 id, Poco::Path basePath) { 
  return {id, std::move(basePath)};
}

Storage::Storage(const Poco::UUID &id, Poco::Path basePath)
    : _id(id), _basePath(std::move(basePath)), _currrentTomId(0) {
  // Create the index path properly
  Poco::File fp(_basePath.makeDirectory());
  if (!fp.exists()) {
    fp.createDirectories();
  }
  std::filesystem::path indexPath = std::filesystem::path(_basePath.toString()) / (_id.toString() + ".linear.storage.index");
  _index = std::make_unique<fc::DiskBTreeMap<MsgUUIDType, Record>>(indexPath, 1024 * 1024, false);

  // Pre-open any existing .tom files so records from a previous run are readable
  Poco::File dir(_basePath);
  if (dir.exists()) {
    std::vector<std::string> files;
    dir.list(files);
    for (const auto& f : files) {
      static const std::string ext = ".tom";
      if (f.size() > ext.size() && f.substr(f.size() - ext.size()) == ext) {
        try {
          Poco::UInt32 tomId = static_cast<Poco::UInt32>(std::stoul(f.substr(0, f.size() - ext.size())));
          if (!_tom.contains(tomId)) {
            _tom.emplace(tomId, Tom(tomId, _basePath));
            if (tomId > _currrentTomId) _currrentTomId = tomId;
          }
        } catch (...) {
          // Skip files that don't parse as numeric tom IDs
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
  if (_tom.contains(ctd)) {
    return _tom.at(ctd);
  }
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
  }
  return ok;
}

// Rebuild the B-tree index by scanning all existing tom files.
// Called after opening existing toms so that record() lookups work correctly.
void Storage::rebuildIndex() {
  for (auto& [tomId, t] : _tom) {
    Poco::UInt64 offset = 0;
    while (true) {
      Record rec = t.record(offset);
      // record() returns default Record (tomId == max) when past end of file
      if (rec.tomId == std::numeric_limits<Poco::UInt32>::max()) break;
      if (rec.header.dataSize == 0) break;  // Safety: no empty records
      if (!rec.header.deleted) {
        (*_index)[rec.header.uuid] = rec;
      }
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
