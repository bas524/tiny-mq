//
// CLIENT_ACKNOWLEDGE and INDIVIDUAL_ACKNOWLEDGE mode tests for tiny-mq
//

#include "ClientAckTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TestHelper.h"
#include <Poco/File.h>

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::TextMessage;

void ClientAckTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void ClientAckTest::TearDown() { _exchange.reset(); }

// AUTO_ACKNOWLEDGE: message is removed from storage automatically on recv (no ack call needed).
TEST_F(ClientAckTest, testAutoAcknowledge) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr dest = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    ASSERT_NE(dest, nullptr);

    Producer::Ptr producer = session.createProducer(dest);
    ASSERT_NE(producer, nullptr);
    Consumer::Ptr consumer = session.createConsumer(dest);
    ASSERT_NE(consumer, nullptr);

    TextMessage msg = session.createTextMessage("auto-ack message", Message::PERSISTENT);
    EXPECT_NO_THROW(producer->send(msg));

    TextMessage::Ptr received = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(msg.text(), received->text());
    // No explicit acknowledgeOn needed in AUTO_ACKNOWLEDGE
}

// CLIENT_ACKNOWLEDGE: persistent message stays in storage until acknowledgeOn is called.
TEST_F(ClientAckTest, testClientAcknowledgePersistent) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
    Destination::Ptr dest = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    ASSERT_NE(dest, nullptr);

    Producer::Ptr producer = session.createProducer(dest);
    ASSERT_NE(producer, nullptr);
    Consumer::Ptr consumer = session.createConsumer(dest);
    ASSERT_NE(consumer, nullptr);

    TextMessage msg1 = session.createTextMessage("first", Message::PERSISTENT);
    TextMessage msg2 = session.createTextMessage("second", Message::PERSISTENT);
    TextMessage msg3 = session.createTextMessage("third", Message::NOT_PERSISTENT);
    EXPECT_NO_THROW(producer->send(msg1));
    EXPECT_NO_THROW(producer->send(msg2));
    EXPECT_NO_THROW(producer->send(msg3));

    TextMessage::Ptr r1 = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ("first", r1->text());

    TextMessage::Ptr r2 = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ("second", r2->text());

    TextMessage::Ptr r3 = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(r3, nullptr);
    EXPECT_EQ("third", r3->text());

    // Acknowledge: removes from storage
    EXPECT_NO_THROW(consumer->acknowledgeOn(*r1));
    EXPECT_NO_THROW(consumer->acknowledgeOn(*r2));
    EXPECT_NO_THROW(consumer->acknowledgeOn(*r3));
}

// Non-persistent messages in AUTO_ACKNOWLEDGE: no storage interaction.
TEST_F(ClientAckTest, testNonPersistentAutoAcknowledge) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr dest = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    ASSERT_NE(dest, nullptr);

    Producer::Ptr producer = session.createProducer(dest);
    Consumer::Ptr consumer = session.createConsumer(dest);

    const size_t count = 10;
    for (size_t i = 0; i < count; ++i) {
        TextMessage msg = session.createTextMessage("msg-" + std::to_string(i), Message::NOT_PERSISTENT);
        EXPECT_NO_THROW(producer->send(msg));
    }
    for (size_t i = 0; i < count; ++i) {
        TextMessage::Ptr received = Message::As<TextMessage>(consumer->recv());
        ASSERT_NE(received, nullptr) << "Expected message " << i;
        EXPECT_EQ("msg-" + std::to_string(i), received->text());
    }
}

// Mixing persistent and non-persistent messages preserves order.
TEST_F(ClientAckTest, testMixedPersistenceOrdering) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr dest = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    ASSERT_NE(dest, nullptr);

    Producer::Ptr producer = session.createProducer(dest);
    Consumer::Ptr consumer = session.createConsumer(dest);

    TextMessage p1 = session.createTextMessage("persistent-1", Message::PERSISTENT);
    TextMessage np = session.createTextMessage("non-persistent", Message::NOT_PERSISTENT);
    TextMessage p2 = session.createTextMessage("persistent-2", Message::PERSISTENT);

    EXPECT_NO_THROW(producer->send(p1));
    EXPECT_NO_THROW(producer->send(np));
    EXPECT_NO_THROW(producer->send(p2));

    TextMessage::Ptr r1 = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(r1, nullptr);
    EXPECT_EQ("persistent-1", r1->text());
    EXPECT_EQ(Message::PERSISTENT, r1->reliability);

    TextMessage::Ptr r2 = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(r2, nullptr);
    EXPECT_EQ("non-persistent", r2->text());
    EXPECT_EQ(Message::NOT_PERSISTENT, r2->reliability);

    TextMessage::Ptr r3 = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(r3, nullptr);
    EXPECT_EQ("persistent-2", r3->text());
    EXPECT_EQ(Message::PERSISTENT, r3->reliability);
}
