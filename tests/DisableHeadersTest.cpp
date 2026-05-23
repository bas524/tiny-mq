//
// Disable MessageID / Timestamp tests for tiny-mq (JMS spec 14)
//

#include "DisableHeadersTest.h"
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

void DisableHeadersTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void DisableHeadersTest::TearDown() { _exchange.reset(); }

// By default the provider assigns a "ID:" messageId and a non-zero timestamp.
TEST_F(DisableHeadersTest, testEnabledByDefault) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage msg = session.createTextMessage("hi");
  producer->send(msg);

  TextMessage::Ptr in = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(in, nullptr);
  EXPECT_FALSE(in->jmsHeaders.messageId.empty());
  EXPECT_EQ(0u, in->jmsHeaders.messageId.rfind("ID:", 0));  // starts with "ID:"
  EXPECT_GT(in->jmsHeaders.timestamp, 0);
}

// setDisableMessageID -> outgoing messageId stays empty.
TEST_F(DisableHeadersTest, testDisableMessageID) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  producer->setDisableMessageID(true);
  EXPECT_TRUE(producer->isDisableMessageID());

  TextMessage msg = session.createTextMessage("hi");
  producer->send(msg);

  TextMessage::Ptr in = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(in, nullptr);
  EXPECT_TRUE(in->jmsHeaders.messageId.empty());
  EXPECT_GT(in->jmsHeaders.timestamp, 0);  // timestamp still set
}

// setDisableMessageTimestamp -> outgoing timestamp stays 0.
TEST_F(DisableHeadersTest, testDisableTimestamp) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  producer->setDisableMessageTimestamp(true);
  EXPECT_TRUE(producer->isDisableMessageTimestamp());

  TextMessage msg = session.createTextMessage("hi");
  producer->send(msg);

  TextMessage::Ptr in = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(in, nullptr);
  EXPECT_EQ(0, in->jmsHeaders.timestamp);
  EXPECT_FALSE(in->jmsHeaders.messageId.empty());  // messageId still set
}
