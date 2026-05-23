//
// DUPS_OK_ACKNOWLEDGE tests for tiny-mq (JMS spec 22)
//
// Note: batching is internal (storage removals are coalesced and flushed at a
// threshold / on consumer teardown). There is no public storage-introspection
// hook yet, so these tests assert end-to-end behaviour and exercise the batch
// + flush code paths; deeper "not removed until flush" assertions await a
// storage-introspection API.

#include "DupsOkAckTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TestHelper.h"

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::TextMessage;

void DupsOkAckTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void DupsOkAckTest::TearDown() { _exchange.reset(); }

// The mode name is exposed correctly.
TEST_F(DupsOkAckTest, testModeName) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::DUPS_OK_ACKNOWLEDGE);
  EXPECT_EQ(Session::AcknowledgeMode::DUPS_OK_ACKNOWLEDGE, session.acknowledgeMode());
  EXPECT_EQ("DUPS_OK_ACKNOWLEDGE", session.acknowledgeModeName());
}

// All persistent messages are delivered and acked under DUPS_OK without error.
TEST_F(DupsOkAckTest, testReceivesAllAndAcks) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::DUPS_OK_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  const int n = 5;
  for (int i = 0; i < n; ++i) {
    producer->send(session.createTextMessage("m-" + std::to_string(i), Message::PERSISTENT));
  }
  for (int i = 0; i < n; ++i) {
    TextMessage::Ptr in = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(in, nullptr) << "missing message " << i;
    EXPECT_EQ("m-" + std::to_string(i), in->text());
    EXPECT_NO_THROW(consumer->acknowledgeOn(*in));
  }
}

// Send more than the batch threshold (100) to exercise the mid-stream flush plus
// the destructor flush. All messages must still be delivered in order.
TEST_F(DupsOkAckTest, testThresholdFlush) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::DUPS_OK_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  const int n = 150;  // > kDupsOkBatch
  for (int i = 0; i < n; ++i) {
    producer->send(session.createTextMessage("x-" + std::to_string(i), Message::PERSISTENT));
  }
  for (int i = 0; i < n; ++i) {
    TextMessage::Ptr in = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(in, nullptr) << "missing message " << i;
    EXPECT_EQ("x-" + std::to_string(i), in->text());
    EXPECT_NO_THROW(consumer->acknowledgeOn(*in));
  }
}
