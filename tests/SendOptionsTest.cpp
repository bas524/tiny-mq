//
// Per-send DeliveryMode / Priority / TTL tests for tiny-mq (JMS spec 12).
//

#include "SendOptionsTest.h"
#include "Connection.h"
#include "Exchange.h"
#include "Producer.h"
#include "Session.h"
#include "TestHelper.h"
#include <Poco/Timestamp.h>
#include <stdexcept>

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::SendOptions;
using tiny_mq::Session;
using tiny_mq::TextMessage;

void SendOptionsTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void SendOptionsTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

static int64_t nowMs() { return Poco::Timestamp().epochMicroseconds() / 1000; }

static TextMessage::Ptr tryRecv(Consumer &c, int64_t usec = 200000) {
  return Message::As<TextMessage>(c.recv(usec));
}

// A per-send opts argument overrides the producer default; a subsequent plain
// send falls back to the stored default.
TEST_F(SendOptionsTest, testPerSendOverrideBeatsDefault) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  producer->setDefault(SendOptions{Message::NOT_PERSISTENT, 2, 0, 0});

  TextMessage m1 = session.createTextMessage("override");
  producer->send(m1, SendOptions{Message::NOT_PERSISTENT, 7, 0, 0});
  auto r1 = tryRecv(*consumer);
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(7, r1->jmsHeaders.priority) << "per-send opts must win";

  TextMessage m2 = session.createTextMessage("default");
  producer->send(m2);
  auto r2 = tryRecv(*consumer);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(2, r2->jmsHeaders.priority) << "plain send must fall back to producer default";
}

// A producer default persists across multiple plain sends.
TEST_F(SendOptionsTest, testDefaultPersistsAcrossSends) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  producer->setDefault(SendOptions{Message::NOT_PERSISTENT, 6, 0, 0});

  for (int i = 0; i < 3; ++i) {
    TextMessage m = session.createTextMessage("n");
    producer->send(m);
    auto r = tryRecv(*consumer);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(6, r->jmsHeaders.priority) << "default must persist on send #" << i;
  }
}

// timeToLive sets JMSExpiration to send-time + ttl; ttl == 0 leaves it unset.
TEST_F(SendOptionsTest, testTimeToLiveSetsExpiration) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  const int64_t ttl = 100000;  // 100 s
  const int64_t before = nowMs();
  TextMessage m = session.createTextMessage("ttl");
  producer->send(m, SendOptions{Message::NOT_PERSISTENT, 4, ttl, 0});
  const int64_t after = nowMs();

  auto r = tryRecv(*consumer);
  ASSERT_NE(r, nullptr);
  EXPECT_GE(r->jmsHeaders.expiration, before + ttl);
  EXPECT_LE(r->jmsHeaders.expiration, after + ttl);

  TextMessage m2 = session.createTextMessage("no-ttl");
  producer->send(m2, SendOptions{Message::NOT_PERSISTENT, 4, 0, 0});
  auto r2 = tryRecv(*consumer);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(0, r2->jmsHeaders.expiration) << "ttl==0 means never expire";
}

// deliveryMode in opts overrides the message's create-time reliability: a
// message created NOT_PERSISTENT is persisted (and so buffered for an offline
// durable subscriber) when sent with PERSISTENT opts.
TEST_F(SendOptionsTest, testDeliveryModeOverridePersists) {
  const std::string subName = "sub-delivmode";
  {
    Connection c(*_exchange);
    Session &s = c.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    ASSERT_NE(s.createDurableConsumer(topic, subName), nullptr);  // goes offline at scope end
  }
  {
    Connection c(*_exchange);
    Session &s = c.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    Producer::Ptr producer = s.createProducer(topic);
    // Message created NOT_PERSISTENT, but opts force PERSISTENT.
    TextMessage m = s.createTextMessage("delivmode", Message::NOT_PERSISTENT);
    producer->send(m, SendOptions{Message::PERSISTENT, 5, 0, 0});
  }
  {
    Connection c(*_exchange);
    Session &s = c.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    Consumer::Ptr sub = s.createDurableConsumer(topic, subName);
    auto r = tryRecv(*sub, 500000);
    ASSERT_NE(r, nullptr) << "PERSISTENT opts must persist the message for the offline durable sub";
    EXPECT_EQ("delivmode", r->text());
    EXPECT_EQ(5, r->jmsHeaders.priority) << "priority must round-trip through storage";
    s.unsubscribe(topic, subName);
  }
}

// Invalid priority / negative ttl are rejected.
TEST_F(SendOptionsTest, testInvalidOptionsThrow) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);

  EXPECT_THROW(producer->setDefault(SendOptions{Message::NOT_PERSISTENT, 10, 0, 0}), std::invalid_argument);
  EXPECT_THROW(producer->setDefault(SendOptions{Message::NOT_PERSISTENT, -1, 0, 0}), std::invalid_argument);

  TextMessage m = session.createTextMessage("bad");
  EXPECT_THROW(producer->send(m, SendOptions{Message::NOT_PERSISTENT, 99, 0, 0}), std::invalid_argument);
  EXPECT_THROW(producer->send(m, SendOptions{Message::NOT_PERSISTENT, 4, -5, 0}), std::invalid_argument);
}

// Without setDefault, a plain send leaves the message's create-time reliability
// and priority untouched (backward compatibility).
TEST_F(SendOptionsTest, testNoDefaultPreservesMessage) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("plain");
  m.jmsHeaders.priority = 8;  // caller-set
  producer->send(m);

  auto r = tryRecv(*consumer);
  ASSERT_NE(r, nullptr);
  EXPECT_EQ(8, r->jmsHeaders.priority) << "no producer default => message priority preserved";
}
