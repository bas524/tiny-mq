//
// Created by Alexander Bychuk on 05.03.2022.
//

#include <numeric>
#include "BytesMessageTest.h"
#include "BytesMessage.h"

using tiny_mq::BytesMessage;
using tiny_mq::BytesVector;

TEST_F(BytesMessageTest, testGetDataLength) {
  BytesMessage msg;

  const size_t len = 10;
  // setBytes is asked to read sizeof(uint32_t) * len bytes (see the
  // dataSize() expectation below), so the source buffer must actually hold
  // that many bytes. It previously held only `len` bytes, so setBytes read
  // sizeof(uint32_t) * len - len = 30 bytes past the end of `data` — a
  // heap-buffer-overflow only visible under ASan (caught incidentally while
  // making the full suite ASan-clean for spec 23 round 4; unrelated to
  // spec 23's own functionality).
  BytesVector data(len * sizeof(uint32_t), 1);

  EXPECT_NO_THROW(msg.setBytes((const int8_t *)&data[0], sizeof(uint32_t) * len));

  size_t resLen = msg.dataSize();
  EXPECT_EQ(resLen, (len * sizeof(uint32_t)));
}

TEST_F(BytesMessageTest, testReadAllBytes) {
  BytesMessage msg;

  int8_t data[50];
  for (int8_t i = 0; i < 50; i++) {
    data[i] = i;
  }
  EXPECT_NO_THROW(msg.writeBytes(&data[0], 0, 50));
  EXPECT_NO_THROW({
    auto refOnBytes = msg.readBytes(0, 50);
    for (int i = 0; i < 50; i++) {
      EXPECT_EQ(refOnBytes[i], i);
    }
  });
}

TEST_F(BytesMessageTest, testReadBytesWithOffset) {
  BytesMessage msg;

  BytesVector data(50);
  std::iota(data.begin(), data.end(), 0);
  EXPECT_NO_THROW(msg.setBytes(data));
  EXPECT_NO_THROW({
    auto refOnBytes = msg.readBytes(24, 25);
    for (auto i : refOnBytes) {
      EXPECT_EQ(refOnBytes[i - 24], i);
    }
  });
}

TEST_F(BytesMessageTest, testWriteBytesWithOffset) {
  BytesMessage msg;

  BytesVector data(10, 0);
  std::iota(data.begin(), data.end(), 0);
  EXPECT_NO_THROW(msg.writeBytes(&data[5], 5, 5));
  EXPECT_NO_THROW({
    auto refOnEmpty = msg.readBytes(0, 5);
    for (auto i : refOnEmpty) {
      EXPECT_EQ(0, i);
    }
    auto refOnBytes = msg.readBytes(5, 5);
    for (auto i : refOnBytes) {
      EXPECT_EQ(refOnBytes[i - 5], i);
    }
  });
}

TEST_F(BytesMessageTest, testWriteBytesWithOffsetText) {
  BytesMessage msg;

  std::string data("0123456789");
  EXPECT_NO_THROW(msg.setBytes(reinterpret_cast<const int8_t *>(data.c_str()), data.size()));
  data = "98765";
  EXPECT_NO_THROW(msg.writeBytes(reinterpret_cast<const int8_t *>(data.c_str()), 5, 5));
  EXPECT_NO_THROW({
    auto refOnData = msg.bytes();
    std::string modifiedData((const char *)refOnData.data(), refOnData.size());
    EXPECT_EQ(modifiedData, "0123498765");
  });
}

TEST_F(BytesMessageTest, testClearBody) {
  BytesMessage msg;
  BytesVector data(50);
  std::iota(data.begin(), data.end(), 0);

  EXPECT_NO_THROW(msg.setBytes(data));

  EXPECT_EQ(msg.bytes(), data);

  msg.clearData();

  EXPECT_TRUE(msg.bytes().empty());
}

TEST_F(BytesMessageTest, testToJson) {
  BytesMessage msg;
  BytesVector data(5);
  std::iota(data.begin(), data.end(), 0);

  EXPECT_NO_THROW(msg.setBytes(data));
  EXPECT_EQ(msg.bytes(), data);
  EXPECT_NO_THROW(msg.setProperty("prop-int", tiny_mq::property::Int(22)));
  EXPECT_NO_THROW(msg.setProperty("prop-null-bytes", tiny_mq::property::Bytes()));

  std::string json;
  EXPECT_NO_THROW(json = tiny_mq::message::dump(msg));
  EXPECT_EQ(json,
            "{\n \"data\": \"AAECAwQ=\",\n \"number\": 0,\n \"persistentInfo\": {\n  \"payload\": {\n   \"dataPath\": \"\",\n   \"propertiesPath\": "
            "\"\"\n  },\n  \"sent\": {\n   \"dataPath\": \"\",\n   \"propertiesPath\": \"\"\n  },\n  \"transaction\": {\n   \"dataPath\": \"\",\n   "
            "\"propertiesPath\": \"\"\n  }\n },\n \"properties\": {\n  \"prop-int\": {\n   \"type\": \"INTEGER\",\n   \"value\": 22\n  },\n  "
            "\"prop-null-bytes\": {\n   \"type\": \"BYTE_ARRAY\",\n   \"value\": null\n  }\n },\n \"reliability\": \"NOT_PERSISTENT\",\n "
            "\"uuid\": \"00000000-0000-0000-0000-000000000000\"\n}");
}
