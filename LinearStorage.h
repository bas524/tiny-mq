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
#include <vector>
#include <unordered_map>
#include <fc/disk_btree.h>

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

class Tom {
  Poco::UInt32 _id{0};
  Poco::Path _basePath;
  Poco::UInt64 _size{0};
  std::unique_ptr<Poco::FileStream> _tomF;

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
  bool remove(Record &record);

  static Tom create(Poco::UInt32 id, Poco::Path basePath);
};

class Storage {
  Poco::UUID _id;
  Poco::Path _basePath;
  Poco::UInt32 _currrentTomId;
  std::unordered_map<Poco::UInt32, Tom> _tom;
  std::unique_ptr<fc::DiskBTreeMap<MsgUUIDType, Record>> _index;

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
};

}  // namespace linear_storage
#endif  // LINEAR_STORAGE_H
