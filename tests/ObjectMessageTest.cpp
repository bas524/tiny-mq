//
// ObjectMessage tests for tiny-mq (JMS spec 11)
//

#include "ObjectMessageTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "ObjectMessage.h"
#include "TestHelper.h"

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::ObjectMessage;
using tiny_mq::Producer;
using tiny_mq::Session;

void ObjectMessageTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void ObjectMessageTest::TearDown() { _exchange.reset(); }

static tiny_mq::BytesVector bv(std::initializer_list<int8_t> b) { return tiny_mq::BytesVector(b); }

// Pure serialization round-trip: body + className survive toBytes/fromBytes.
TEST_F(ObjectMessageTest, testToBytesRoundTrip) {
  ObjectMessage msg(Poco::UUIDGenerator().createRandom(), bv({1, 2, 3, 4, 5}), "com.example.Order");
  auto bytes = msg.toBytes();

  ObjectMessage restored;
  restored.fromBytes(bytes);
  EXPECT_EQ("com.example.Order", restored.className());
  EXPECT_EQ(msg.body(), restored.body());
}

// Empty body and empty className are preserved.
TEST_F(ObjectMessageTest, testEmptyBody) {
  ObjectMessage msg(Poco::UUIDGenerator().createRandom(), {}, "");
  auto bytes = msg.toBytes();
  ObjectMessage restored;
  restored.fromBytes(bytes);
  EXPECT_TRUE(restored.className().empty());
  EXPECT_TRUE(restored.body().empty());
}

// End-to-end persistent send/recv keeps body + className through storage.
TEST_F(ObjectMessageTest, testPersistentSendRecv) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  ObjectMessage out = session.createObjectMessage(bv({9, 8, 7}), "com.example.Event", Message::PERSISTENT);
  producer->send(out);

  ObjectMessage::Ptr in = Message::As<ObjectMessage>(consumer->recv());
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(Message::OBJECT_MESSAGE, in->type());
  EXPECT_EQ("com.example.Event", in->className());
  EXPECT_EQ(out.body(), in->body());
  consumer->acknowledgeOn(*in);
}
