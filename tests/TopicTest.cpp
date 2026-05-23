//
// Topic (pub/sub) tests for tiny-mq
//

#include "TopicTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TestHelper.h"

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::TextMessage;

void TopicTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void TopicTest::TearDown() { _exchange.reset(); }

// Each subscriber on a topic receives its own copy of every published message.
TEST_F(TopicTest, testPublishToMultipleSubscribers) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    ASSERT_NE(topic, nullptr);

    // Create two independent subscribers before publishing
    Consumer::Ptr sub1 = session.createConsumer(topic);
    ASSERT_NE(sub1, nullptr);
    Consumer::Ptr sub2 = session.createConsumer(topic);
    ASSERT_NE(sub2, nullptr);

    Producer::Ptr producer = session.createProducer(topic);
    ASSERT_NE(producer, nullptr);

    TextMessage msg = session.createTextMessage("hello topic");
    producer->send(msg);

    TextMessage::Ptr recv1 = Message::As<TextMessage>(sub1->recv());
    ASSERT_NE(recv1, nullptr) << "Subscriber 1 should receive the message";
    EXPECT_EQ(msg.text(), recv1->text());

    TextMessage::Ptr recv2 = Message::As<TextMessage>(sub2->recv());
    ASSERT_NE(recv2, nullptr) << "Subscriber 2 should receive the message";
    EXPECT_EQ(msg.text(), recv2->text());
}

// Publishing multiple messages preserves per-subscriber ordering.
TEST_F(TopicTest, testPublishOrdering) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    ASSERT_NE(topic, nullptr);

    Consumer::Ptr sub = session.createConsumer(topic);
    ASSERT_NE(sub, nullptr);

    Producer::Ptr producer = session.createProducer(topic);
    ASSERT_NE(producer, nullptr);

    const size_t count = 5;
    for (size_t i = 0; i < count; ++i) {
        TextMessage msg = session.createTextMessage("msg-" + std::to_string(i));
        producer->send(msg);
    }

    for (size_t i = 0; i < count; ++i) {
        TextMessage::Ptr received = Message::As<TextMessage>(sub->recv());
        ASSERT_NE(received, nullptr);
        EXPECT_EQ("msg-" + std::to_string(i), received->text());
    }
}

// A subscriber created after a message is published should not receive it.
TEST_F(TopicTest, testLateSubscriberMissesEarlierMessages) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    ASSERT_NE(topic, nullptr);

    // First subscriber created before publish
    Consumer::Ptr sub1 = session.createConsumer(topic);
    ASSERT_NE(sub1, nullptr);

    Producer::Ptr producer = session.createProducer(topic);
    ASSERT_NE(producer, nullptr);

    TextMessage msg = session.createTextMessage("early message");
    producer->send(msg);

    // sub1 can receive it
    TextMessage::Ptr recv1 = Message::As<TextMessage>(sub1->recv());
    ASSERT_NE(recv1, nullptr);
    EXPECT_EQ(msg.text(), recv1->text());

    // Late subscriber — must NOT receive the already-published message
    Consumer::Ptr sub2 = session.createConsumer(topic);
    ASSERT_NE(sub2, nullptr);

    TextMessage::Ptr shouldBeNull = Message::As<TextMessage>(sub2->recv(50000));
    EXPECT_EQ(nullptr, shouldBeNull) << "Late subscriber must not receive earlier messages";
}

// Transacted topic publish: messages only delivered after commit.
TEST_F(TopicTest, testTransactedTopicPublish) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    ASSERT_NE(topic, nullptr);

    Consumer::Ptr sub = session.createConsumer(topic);
    ASSERT_NE(sub, nullptr);

    Producer::Ptr producer = session.createProducer(topic);
    ASSERT_NE(producer, nullptr);

    TextMessage msg = session.createTextMessage("transacted topic message");
    EXPECT_NO_THROW(producer->send(msg));

    // Before commit, subscriber should not see the message
    TextMessage::Ptr beforeCommit = Message::As<TextMessage>(sub->recv(50000));
    EXPECT_EQ(nullptr, beforeCommit) << "Message must not be visible before commit";

    EXPECT_NO_THROW(session.commit());

    TextMessage::Ptr afterCommit = Message::As<TextMessage>(sub->recv());
    ASSERT_NE(afterCommit, nullptr) << "Message must be visible after commit";
    EXPECT_EQ(msg.text(), afterCommit->text());

    EXPECT_NO_THROW(session.commit());
}
