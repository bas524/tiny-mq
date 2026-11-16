//
// Created by Alexander Bychuk on 16.09.2023.
//

#ifndef LINEAR_STORAGE_H
#define LINEAR_STORAGE_H

#include <Poco/Types.h>
#include <Poco/UUID.h>
#include <Poco/FileStream.h>
#include <Poco/Path.h>
#include <memory>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <fc/disk_btree.h>

namespace tiny_mq { class LocalTransactionJournal; }

namespace linear_storage {

namespace fc = frozenca;
using MsgUUIDType = std::array<char, 16>;

struct Header {
  /// uuid - record unique id
  MsgUUIDType uuid = {};
  /// dataSize - size of data which next to record header
  Poco::UInt64 dataSize{0};
  /// deleted - flag shows that record is alive or removed
  Poco::Int8 deleted{0};
  // First byte of legacy tail padding, used only in marked delivery volumes.
  uint8_t attempt = 0;
  std::array<unsigned char, 6> reserved{};

  friend bool operator==(const Header &lhs, const Header &rhs) {
    return (lhs.deleted == rhs.deleted) && (lhs.dataSize == rhs.dataSize) && (memcmp(&lhs.uuid[0], &rhs.uuid[0], sizeof(lhs.uuid)) == 0);
  }
};

struct Record {
  Poco::UInt32 tomId{std::numeric_limits<Poco::UInt32>::max()};
  Poco::UInt64 offset{std::numeric_limits<Poco::UInt64>::max()};
  Header header;
  friend bool operator==(const Record &lhs, const Record &rhs) {
    return (lhs.tomId == rhs.tomId) && (lhs.offset == rhs.offset) && (lhs.header == rhs.header);
  }
};

// Resume point for a chunked prefix scan (see Storage::scanPrefix): a full pass
// is split across calls so no single call walks the whole store at once.
struct SweepCursor {
  Poco::UInt32 tomId{0};
  Poco::UInt64 offset{0};
};

class Tom {
  friend class Storage;
  friend class tiny_mq::LocalTransactionJournal;
  Poco::UInt32 _id{0};
  Poco::Path _basePath;
  Poco::UInt64 _size{0};
#if defined(_WIN32)
  std::unique_ptr<Poco::FileStream> _tomF;
#else
  int _fd = -1;
  char* _mapped = nullptr;
  size_t _mappedSize = 0;
  Poco::UInt64 _mappedAt = 0;
  void unmap() noexcept;
#endif
  bool _deliveryVolume = false;
  bool writeAt(const void* data, size_t size, Poco::UInt64 offset);
  bool readAt(void* data, size_t size, Poco::UInt64 offset) const;
  bool setFlag(Poco::UInt64 offset);
  void markAttempt(Poco::UInt64 offset);
  void open();
  void truncate(Poco::UInt64 size);

 public:
  Tom(Poco::UInt32 id, Poco::Path basePath);
  Tom(const Tom &) = delete;
  Tom(Tom &&) noexcept ;
  Tom &operator=(const Tom &) = delete;
  Tom &operator=(Tom &&) = delete;
  ~Tom() noexcept;
  [[nodiscard]] Poco::UInt32 id() const;
  Record append(const Poco::UUID &uuid, const std::vector<char> &data);
  [[nodiscard]] Record record(Poco::UInt64 offset) const;
  [[nodiscard]] std::vector<char> data(const Record &record) const;
  // Read only the first `len` bytes of a record's data (clamped to dataSize).
  // Lets callers inspect a fixed header prefix without materialising the payload.
  [[nodiscard]] std::vector<char> dataPrefix(const Record &record, size_t len) const;
  bool remove(Record &record);

  static Tom create(Poco::UInt32 id, Poco::Path basePath);
};

class Storage {
  friend class tiny_mq::LocalTransactionJournal;
  Poco::UUID _id;
  Poco::Path _basePath;
  Poco::UInt32 _currrentTomId;
  std::unordered_map<Poco::UInt32, Tom> _tom;
  std::unique_ptr<fc::DiskBTreeMap<MsgUUIDType, Record>> _index;
  void clearDeliveryAttempt(const Record& record) const noexcept;
  bool _hasAttemptFiles = false;
  std::filesystem::path _directory;

 public:
  Storage(const Poco::UUID &id, Poco::Path basePath);
  ~Storage();
  Tom &currentTom();
  [[nodiscard]] const Tom &tom(Poco::UInt32 id) const;
  Tom &tom(Poco::UInt32 id);
  void rebuildIndex();
  Record append(const Poco::UUID &uuid, const std::vector<char> &data);
  [[nodiscard]] Record record(Poco::UInt32 tomId, Poco::UInt64 offset) const;
  [[nodiscard]] Record record(const Poco::UUID &uuid) const;
  [[nodiscard]] std::vector<char> data(const Record &record) const;
  bool remove(Record &record);
  [[nodiscard]] std::vector<std::pair<Record, std::vector<char>>> scan() const;
  // Like scan(), but reads only the first `prefixLen` bytes of each live record
  // and processes at most `maxRecords` per call, resuming from `cursor`. Reads
  // headers + a small prefix instead of whole payloads, so a sweep never
  // materialises the entire store in RAM. When a full pass completes the cursor
  // is reset to the beginning.
  [[nodiscard]] std::vector<std::pair<Record, std::vector<char>>>
  scanPrefix(size_t prefixLen, size_t maxRecords, SweepCursor &cursor) const;
};

}  // namespace linear_storage
#endif  // LINEAR_STORAGE_H
