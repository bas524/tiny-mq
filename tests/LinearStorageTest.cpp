//
// Created by Alexander Bychuk on 17.09.2023.
//

#include "LinearStorageTest.h"
#include "ConcurrentLinearStorage.h"
#include <Poco/File.h>
#include <Poco/Thread.h>
#include <Poco/UUIDGenerator.h>
#include <Poco/Random.h>
#include <limits>
#include "Destination.h"
#include "Session.h"
#include "TextMessage.h"

class StorageWriter : public Poco::Runnable {
  linear_storage::ConcurrentLinearStorage &_storage;
  char _a;
  linear_storage::Record _recResult;
  std::vector<char> _data;

 public:
  StorageWriter(linear_storage::ConcurrentLinearStorage &storage, char a) : _storage(storage), _a(a) {}
  void run() override {
    Poco::Random rnd;
    rnd.seed();
    auto i = rnd.next(1000);
    Poco::Thread::sleep(i);
    auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
    _data = {'H', 'E', 'L', 'L', 'O', ' ', 'W', 'O', 'R', 'L', 'D', _a};
    _recResult = _storage.append(uuid, _data);
  }
  [[nodiscard]] const linear_storage::Record &recResult() const { return _recResult; }
  [[nodiscard]] const std::vector<char> &data() const { return _data; }
};

void LinearStorageTest::SetUp() { _basePath = ("./tiny-mq/storage-test"); }
void LinearStorageTest::TearDown() {
  Poco::File f(_basePath);
  if (f.exists()) {
    f.remove(true);
  }
}

TEST_F(LinearStorageTest, testSimpleReadWrite) {
  linear_storage::Tom tom(0, _basePath);
  std::vector<char> data = {'H', 'E', 'L', 'L', 'O', ' ', 'W', 'O', 'R', 'L', 'D'};
  auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  linear_storage::Record rec1 = tom.append(uuid, data);
  linear_storage::Record rec2 = tom.record(rec1.offset);
  EXPECT_EQ(rec1, rec2);
  auto readData = tom.data(rec2);
  EXPECT_EQ(data, readData);
}

TEST_F(LinearStorageTest, testSimpleThreadedReadWrite) {
  auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  linear_storage::ConcurrentLinearStorage storage(uuid, _basePath);
  storage.start();
  StorageWriter tw1(storage, '1');
  StorageWriter tw2(storage, '2');
  StorageWriter tw3(storage, '3');
  StorageWriter tw4(storage, '4');
  Poco::Thread thr1("w1");
  Poco::Thread thr2("w2");
  Poco::Thread thr3("w3");
  Poco::Thread thr4("w4");
  thr1.start(tw1);
  thr2.start(tw2);
  thr3.start(tw3);
  thr4.start(tw4);
  thr1.join();
  thr2.join();
  thr3.join();
  thr4.join();

  linear_storage::Record rec1 = storage.record(tw1.recResult().tomId, tw1.recResult().offset);
  linear_storage::Record rec2 = storage.record(tw2.recResult().tomId, tw2.recResult().offset);
  linear_storage::Record rec3 = storage.record(tw3.recResult().tomId, tw3.recResult().offset);
  linear_storage::Record rec4 = storage.record(tw4.recResult().tomId, tw4.recResult().offset);
  EXPECT_EQ(rec1, tw1.recResult());
  EXPECT_EQ(rec2, tw2.recResult());
  EXPECT_EQ(rec3, tw3.recResult());
  EXPECT_EQ(rec4, tw4.recResult());
  auto readData1 = storage.data(rec1);
  auto readData2 = storage.data(rec2);
  auto readData3 = storage.data(rec3);
  auto readData4 = storage.data(rec4);
  EXPECT_EQ(tw1.data(), readData1);
  EXPECT_EQ(tw2.data(), readData2);
  EXPECT_EQ(tw3.data(), readData3);
  EXPECT_EQ(tw4.data(), readData4);
  storage.stop();
}

TEST_F(LinearStorageTest, testSimpleGetRecordByUUID) {
  auto storageUuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  linear_storage::Storage stg(storageUuid, _basePath);
  std::vector<char> data = {'H', 'E', 'L', 'L', 'O', ' ', 'W', 'O', 'R', 'L', 'D', ' ', 'B', 'Y', ' ', 'U', 'U', 'I', 'D'};
  auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  linear_storage::Record rec1 = stg.append(uuid, data);
  linear_storage::Record rec2 = stg.record(rec1.tomId, rec1.offset);
  EXPECT_EQ(rec1, rec2);
  auto readData = stg.data(rec2);
  EXPECT_EQ(data, readData);
  linear_storage::Record rec3 = stg.record(uuid);
  EXPECT_EQ(rec1, rec3);
  auto readDataByUUID = stg.data(rec3);
  EXPECT_EQ(data, readDataByUUID);
}

// Append/read/remove lifecycle — the storage operations behind persistent
// send (append) and acknowledge (remove). After removal the record must be
// unfindable by UUID.
TEST_F(LinearStorageTest, testAppendReadRemoveLifecycle) {
  auto storageUuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  linear_storage::ConcurrentLinearStorage storage(storageUuid, _basePath);
  storage.start();

  std::vector<char> data = {'P', 'A', 'Y', 'L', 'O', 'A', 'D'};
  auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();

  linear_storage::Record rec = storage.append(uuid, data);
  linear_storage::Record byUuid = storage.record(uuid);
  EXPECT_EQ(rec, byUuid);
  EXPECT_EQ(data, storage.data(byUuid));

  // Before removal the live scan contains exactly this record.
  auto before = storage.scan();
  EXPECT_EQ(1u, before.size());

  EXPECT_TRUE(storage.remove(rec));

  // After removal the record is no longer live (scan skips deleted records).
  auto after = storage.scan();
  for (const auto& [r, d] : after) {
    (void)d;
    EXPECT_NE(rec, r) << "removed record must not appear in scan";
  }
  EXPECT_TRUE(after.empty());

  // And it must be unfindable by UUID — the index entry is dropped on remove,
  // not left stale until restart.
  linear_storage::Record afterRemove = storage.record(uuid);
  EXPECT_EQ(std::numeric_limits<Poco::UInt32>::max(), afterRemove.tomId)
      << "record(uuid) must return not-found after remove";

  storage.stop();
}
