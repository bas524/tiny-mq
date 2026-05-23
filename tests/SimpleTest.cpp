//
// Created by Alexander Bychuk on 12.02.2022.
//

#include "SimpleTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TextMessage.h"
#include "TestHelper.h"
#include <Poco/UUIDGenerator.h>

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::TextMessage;

void SimpleTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void SimpleTest::TearDown() { _exchange.reset(); }

////////////////////////////////////////////////////////////////////////////////
TEST_F(SimpleTest, testSendRecv) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, "test");
  EXPECT_NE(destination, nullptr);

  Producer::Ptr producer = session.createProducer(destination);
  EXPECT_NE(producer, nullptr);

  TextMessage message1 = session.createTextMessage("TEST MESSAGE 1");

  TextMessage message2 = session.createTextMessage("TEST MESSAGE 2");

  producer->send(message1);
  producer->send(message2);

  Consumer::Ptr consumer = session.createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  TextMessage::Ptr pmessage1 = Message::As<TextMessage>(consumer->recv());
  EXPECT_NE(pmessage1, nullptr);
  consumer->acknowledgeOn(*pmessage1);
  TextMessage::Ptr pmessage2 = Message::As<TextMessage>(consumer->recv());
  EXPECT_NE(pmessage2, nullptr);
  consumer->acknowledgeOn(*pmessage2);

  EXPECT_EQ(message1.text(), pmessage1->text());
  EXPECT_EQ(message2.text(), pmessage2->text());
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(SimpleTest, testTransactSendRecv) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, "test");
  EXPECT_NE(destination, nullptr);

  Producer::Ptr producer = session.createProducer(destination);
  EXPECT_NE(producer, nullptr);

  Consumer::Ptr consumer = session.createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  TextMessage message1 = session.createTextMessage("TEST MESSAGE 1", Message::PERSISTENT);

  TextMessage message2 = session.createTextMessage("TEST MESSAGE 2");

  producer->send(message1);
  producer->send(message2);

  session.commit();

  TextMessage::Ptr pmessage1 = Message::As<TextMessage>(consumer->recv());
  EXPECT_NE(pmessage1, nullptr);
  consumer->acknowledgeOn(*pmessage1);
  TextMessage::Ptr pmessage2 = Message::As<TextMessage>(consumer->recv());
  EXPECT_NE(pmessage2, nullptr);
  consumer->acknowledgeOn(*pmessage2);

  EXPECT_EQ(message1.text(), pmessage1->text());
  EXPECT_EQ(message2.text(), pmessage2->text());
  session.commit();
}

// M0-2 acceptance: a single connection can run two sessions with different
// acknowledge modes against the same destination, independently.
TEST_F(SimpleTest, testTwoSessionsDifferentAckModesSameDestination) {
  Connection connection(*_exchange);
  Session &txSession = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Session &autoSession = connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);

  EXPECT_EQ(Session::AcknowledgeMode::SESSION_TRANSACTED, txSession.acknowledgeMode());
  EXPECT_EQ(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE, autoSession.acknowledgeMode());

  // Both sessions resolve the same name to the same underlying destination.
  Destination::Ptr dTx = txSession.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Destination::Ptr dAuto = autoSession.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(dTx, nullptr);
  ASSERT_EQ(dTx, dAuto);

  // Transacted producer: nothing is visible until commit.
  Producer::Ptr producer = txSession.createProducer(dTx);
  Consumer::Ptr consumer = autoSession.createConsumer(dAuto);

  producer->send(txSession.createTextMessage("m1"));
  producer->send(txSession.createTextMessage("m2"));

  // Auto-ack consumer must not see uncommitted messages yet.
  EXPECT_EQ(nullptr, Message::As<TextMessage>(consumer->recv(100)));

  txSession.commit();

  TextMessage::Ptr r1 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(r1, nullptr);
  TextMessage::Ptr r2 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ("m1", r1->text());
  EXPECT_EQ("m2", r2->text());
}

TEST_F(SimpleTest, testSaveAndRestoreMessage) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  TextMessage message = session.createTextMessage("TEST MESSAGE 1");
  std::string propName = "test-int";
  message.setProperty(propName, tiny_mq::property::Int(42));
  auto payload = message.dataAsBytes();
  EXPECT_FALSE(payload.empty());
  auto props = message.propertiesAsBytes();
  EXPECT_FALSE(props.empty());
  TextMessage newMessage = session.createTextMessage("");
  EXPECT_NO_THROW(newMessage.setDataFromBytes(payload));
  EXPECT_NO_THROW(newMessage.setPropertiesFromBytes(props));
  EXPECT_EQ(message.text(), newMessage.text());
  EXPECT_EQ(message.property<tiny_mq::property::Int>(propName).value(), newMessage.property<tiny_mq::property::Int>(propName).value());
}
