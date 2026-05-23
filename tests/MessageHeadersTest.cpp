//
// Created by Alexander Bychuk on 24.04.2026.
//

#include "MessageHeadersTest.h"
#include "TextMessage.h"
#include <Poco/UUIDGenerator.h>

using tiny_mq::TextMessage;
using tiny_mq::Message;
using tiny_mq::BytesVector;

namespace {
TextMessage makeSample() {
  TextMessage msg({}, "payload");
  msg.uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  msg.reliability = Message::PERSISTENT;
  msg.jmsHeaders.messageId     = "ID:custom-42";
  msg.jmsHeaders.timestamp     = 1700000000123LL;
  msg.jmsHeaders.expiration    = 1700000999999LL;
  msg.jmsHeaders.deliveryTime  = 1700000000500LL;
  msg.jmsHeaders.priority      = 7;
  msg.jmsHeaders.deliveryCount = 3;
  msg.jmsHeaders.redelivered   = true;
  msg.jmsHeaders.replyTo       = "queue://replies";
  msg.jmsHeaders.correlationId = "corr-xyz";
  msg.jmsHeaders.type          = "OrderEvent";
  return msg;
}
}  // namespace

TEST_F(MessageHeadersTest, defaultHeadersAreZero) {
  TextMessage msg;
  EXPECT_TRUE(msg.jmsHeaders.messageId.empty());
  EXPECT_EQ(msg.jmsHeaders.timestamp, 0);
  EXPECT_EQ(msg.jmsHeaders.expiration, 0);
  EXPECT_EQ(msg.jmsHeaders.deliveryTime, 0);
  EXPECT_EQ(msg.jmsHeaders.priority, 4);  // JMS default
  EXPECT_EQ(msg.jmsHeaders.deliveryCount, 0);
  EXPECT_FALSE(msg.jmsHeaders.redelivered);
  EXPECT_TRUE(msg.jmsHeaders.replyTo.empty());
  EXPECT_TRUE(msg.jmsHeaders.correlationId.empty());
  EXPECT_TRUE(msg.jmsHeaders.type.empty());
}

TEST_F(MessageHeadersTest, roundTripAllHeadersThroughBytes) {
  TextMessage original = makeSample();
  BytesVector bytes = original.toBytes();

  TextMessage restored;
  restored.fromBytes(bytes);

  EXPECT_EQ(restored.uuid, original.uuid);
  EXPECT_EQ(restored.reliability, Message::PERSISTENT);
  EXPECT_EQ(restored.jmsHeaders.messageId,     original.jmsHeaders.messageId);
  EXPECT_EQ(restored.jmsHeaders.timestamp,     original.jmsHeaders.timestamp);
  EXPECT_EQ(restored.jmsHeaders.expiration,    original.jmsHeaders.expiration);
  EXPECT_EQ(restored.jmsHeaders.deliveryTime,  original.jmsHeaders.deliveryTime);
  EXPECT_EQ(restored.jmsHeaders.priority,      original.jmsHeaders.priority);
  EXPECT_EQ(restored.jmsHeaders.deliveryCount, original.jmsHeaders.deliveryCount);
  EXPECT_EQ(restored.jmsHeaders.redelivered,   original.jmsHeaders.redelivered);
  EXPECT_EQ(restored.jmsHeaders.replyTo,       original.jmsHeaders.replyTo);
  EXPECT_EQ(restored.jmsHeaders.correlationId, original.jmsHeaders.correlationId);
  EXPECT_EQ(restored.jmsHeaders.type,          original.jmsHeaders.type);
  EXPECT_EQ(restored.text(), original.text());
}

TEST_F(MessageHeadersTest, roundTripPreservesPayloadAndProperties) {
  TextMessage msg({}, "big payload with headers");
  msg.uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  msg.setIntProperty("n", 42);
  msg.setStringProperty("tag", std::string("alpha"));
  msg.jmsHeaders.messageId = "ID:xyz";
  msg.jmsHeaders.priority  = 9;

  TextMessage restored;
  restored.fromBytes(msg.toBytes());

  EXPECT_EQ(restored.text(), "big payload with headers");
  EXPECT_EQ(restored.property<tiny_mq::property::Int>("n").value(), 42);
  EXPECT_EQ(restored.property<tiny_mq::property::String>("tag").value(), "alpha");
  EXPECT_EQ(restored.jmsHeaders.messageId, "ID:xyz");
  EXPECT_EQ(restored.jmsHeaders.priority, 9);
}

TEST_F(MessageHeadersTest, emptyStringHeadersStayEmpty) {
  TextMessage msg({}, "p");
  msg.jmsHeaders.messageId = "";
  msg.jmsHeaders.replyTo   = "";

  TextMessage restored;
  restored.fromBytes(msg.toBytes());

  EXPECT_TRUE(restored.jmsHeaders.messageId.empty());
  EXPECT_TRUE(restored.jmsHeaders.replyTo.empty());
}

TEST_F(MessageHeadersTest, legacyV1FormatReadsWithDefaultHeaders) {
  // Hand-craft a 0x01 record: [magic=1][number=7][uuid][reliability=1][propsLen=0][data="hi"]
  BytesVector v;
  v.push_back(static_cast<int8_t>(0x01));

  int64_t number = 7;
  auto appendBytes = [&v](const void* p, size_t n) {
    auto b = static_cast<const int8_t*>(p);
    v.insert(v.end(), b, b + n);
  };
  appendBytes(&number, sizeof(number));

  Poco::UUID uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  char uuidBuf[16]; uuid.copyTo(uuidBuf);
  appendBytes(uuidBuf, 16);

  v.push_back(static_cast<int8_t>(1));  // reliability = PERSISTENT
  uint32_t propsLen = 0;
  appendBytes(&propsLen, sizeof(propsLen));
  const char data[] = "hi";
  appendBytes(data, 2);

  TextMessage restored;
  restored.fromBytes(v);

  EXPECT_EQ(restored.uuid, uuid);
  EXPECT_EQ(restored.reliability, Message::PERSISTENT);
  EXPECT_EQ(restored.text(), "hi");
  // 0x01 had no headers — expect all-default Headers.
  EXPECT_TRUE(restored.jmsHeaders.messageId.empty());
  EXPECT_EQ(restored.jmsHeaders.timestamp, 0);
  EXPECT_EQ(restored.jmsHeaders.priority, 4);
  EXPECT_FALSE(restored.jmsHeaders.redelivered);
}
