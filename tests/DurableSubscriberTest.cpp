// Durable subscriber tests for tiny-mq

#include "DurableSubscriberTest.h"
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

void DurableSubscriberTest::SetUp() {
    _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq");
}
void DurableSubscriberTest::TearDown() { _exchange.reset(); }

// ─── Helpers ────────────────────────────────────────────────────────────────

// Receive a TextMessage or return nullptr after a short timeout (50 ms).
static TextMessage::Ptr tryRecv(Consumer &c, int64_t usec = 50000) {
    return Message::As<TextMessage>(c.recv(usec));
}

// ─── Tests ──────────────────────────────────────────────────────────────────

// Durable subscriber receives messages published while it is online, just like
// a regular topic subscriber.
TEST_F(DurableSubscriberTest, testOnlineReceive) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);
    ASSERT_NE(topic, nullptr);

    Consumer::Ptr sub = session.createDurableConsumer(topic, "sub-online");
    ASSERT_NE(sub, nullptr);

    Producer::Ptr producer = session.createProducer(topic);
    ASSERT_NE(producer, nullptr);

    auto msg = session.createTextMessage("hello durable", Message::PERSISTENT);
    producer->send(msg);

    auto recv = tryRecv(*sub);
    ASSERT_NE(recv, nullptr);
    EXPECT_EQ(msg.text(), recv->text());

    session.unsubscribe(topic, "sub-online");
}

// Persistent messages published while the durable subscriber is offline must be
// delivered when it reconnects.
TEST_F(DurableSubscriberTest, testOfflineBufferingAndReconnect) {
    const std::string subName = "sub-offline";

    {
        // Session 1: create the durable subscription then go offline
        Connection session1_conn(*_exchange);
        Session &session1 = session1_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = session1.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = session1.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);
        // Consumer goes offline when session1 is destroyed
    }

    {
        // Session 2: publish two persistent messages while subscriber is offline
        Connection session2_conn(*_exchange);
        Session &session2 = session2_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = session2.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Producer::Ptr producer = session2.createProducer(topic);
        ASSERT_NE(producer, nullptr);

        producer->send(session2.createTextMessage("offline-msg-1", Message::PERSISTENT));
        producer->send(session2.createTextMessage("offline-msg-2", Message::PERSISTENT));
    }

    {
        // Session 3: reconnect with the same subscription name, buffered messages arrive
        Connection session3_conn(*_exchange);
        Session &session3 = session3_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = session3.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = session3.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);

        auto r1 = tryRecv(*sub, 500000);
        ASSERT_NE(r1, nullptr) << "First offline message must be delivered on reconnect";
        EXPECT_EQ("offline-msg-1", r1->text());

        auto r2 = tryRecv(*sub, 500000);
        ASSERT_NE(r2, nullptr) << "Second offline message must be delivered on reconnect";
        EXPECT_EQ("offline-msg-2", r2->text());

        session3.unsubscribe(topic, subName);
    }
}

// Non-persistent messages must NOT be buffered for offline durable subscribers.
TEST_F(DurableSubscriberTest, testNonPersistentNotBuffered) {
    const std::string subName = "sub-nonpersist";

    {
        Connection session1_conn(*_exchange);
        Session &session1 = session1_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = session1.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = session1.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);
    }

    {
        Connection session2_conn(*_exchange);
        Session &session2 = session2_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = session2.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Producer::Ptr producer = session2.createProducer(topic);

        // Send a non-persistent message while subscriber is offline
        producer->send(session2.createTextMessage("volatile-msg", Message::NOT_PERSISTENT));
    }

    {
        Connection session3_conn(*_exchange);
        Session &session3 = session3_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = session3.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = session3.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);

        auto r = tryRecv(*sub);
        EXPECT_EQ(nullptr, r) << "Non-persistent messages must not be buffered for offline durable subs";

        session3.unsubscribe(topic, subName);
    }
}

// Unsubscribing removes the durable subscription; a subsequent reconnect with
// the same name creates a fresh subscription (no stale buffered messages).
TEST_F(DurableSubscriberTest, testUnsubscribeRemovesSubscription) {
    const std::string subName = "sub-unsub";

    {
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = s.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);
    }

    {
        // Publish while offline
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Producer::Ptr producer = s.createProducer(topic);
        producer->send(s.createTextMessage("should-be-lost", Message::PERSISTENT));

        // Unsubscribe — this discards the buffered message
        s.unsubscribe(topic, subName);
    }

    {
        // A new subscription with the same name starts fresh
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = s.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);

        auto r = tryRecv(*sub);
        EXPECT_EQ(nullptr, r) << "After unsubscribe, no old messages should be present";

        s.unsubscribe(topic, subName);
    }
}

// Two independent durable subscriptions on the same topic receive independent
// copies of each published message.
TEST_F(DurableSubscriberTest, testMultipleDurableSubscribers) {
    const std::string subA = "sub-multi-a";
    const std::string subB = "sub-multi-b";

    // Both subscriptions go offline
    {
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        s.createDurableConsumer(topic, subA);
        s.createDurableConsumer(topic, subB);
    }

    // Publish while both are offline
    {
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Producer::Ptr producer = s.createProducer(topic);
        producer->send(s.createTextMessage("shared-msg", Message::PERSISTENT));
    }

    // Both subscriptions should independently receive the message on reconnect
    {
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);

        Consumer::Ptr consA = s.createDurableConsumer(topic, subA);
        Consumer::Ptr consB = s.createDurableConsumer(topic, subB);

        auto rA = tryRecv(*consA, 500000);
        ASSERT_NE(rA, nullptr) << "Subscription A must receive the buffered message";
        EXPECT_EQ("shared-msg", rA->text());

        auto rB = tryRecv(*consB, 500000);
        ASSERT_NE(rB, nullptr) << "Subscription B must receive the buffered message";
        EXPECT_EQ("shared-msg", rB->text());

        s.unsubscribe(topic, subA);
        s.unsubscribe(topic, subB);
    }
}

// A durable subscription is identified by (clientID, name): the same name on the
// same topic under two different clientIDs yields two independent subscriptions,
// each with its own offline buffer (JMS 2.0 § 6.3 / spec 03).
TEST_F(DurableSubscriberTest, testClientIdScopesDurableSubscription) {
    const std::string subName = "shared-name";

    // Two clients register the same subscription name on the same topic, then
    // both go offline.
    {
        Connection connA(*_exchange);
        connA.setClientID("client-A");
        Session &sA = connA.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = sA.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        ASSERT_NE(sA.createDurableConsumer(topic, subName), nullptr);

        Connection connB(*_exchange);
        connB.setClientID("client-B");
        Session &sB = connB.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topicB = sB.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        // Same name under a different clientID must NOT collide with client-A's.
        ASSERT_NE(sB.createDurableConsumer(topicB, subName), nullptr);
    }

    // Publish one persistent message while both subscriptions are offline.
    {
        Connection pub(*_exchange);
        Session &s = pub.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Producer::Ptr producer = s.createProducer(topic);
        producer->send(s.createTextMessage("scoped-msg", Message::PERSISTENT));
    }

    // Each client independently buffered the message and receives its own copy.
    {
        Connection connA(*_exchange);
        connA.setClientID("client-A");
        Session &sA = connA.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = sA.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr consA = sA.createDurableConsumer(topic, subName);
        auto rA = tryRecv(*consA, 500000);
        ASSERT_NE(rA, nullptr) << "client-A's subscription must keep its own buffered copy";
        EXPECT_EQ("scoped-msg", rA->text());

        Connection connB(*_exchange);
        connB.setClientID("client-B");
        Session &sB = connB.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topicB = sB.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr consB = sB.createDurableConsumer(topicB, subName);
        auto rB = tryRecv(*consB, 500000);
        ASSERT_NE(rB, nullptr) << "client-B's subscription is independent and keeps its own copy";
        EXPECT_EQ("scoped-msg", rB->text());

        // Unsubscribing client-A's subscription must not disturb client-B's.
        sA.unsubscribe(topic, subName);
        auto stillThere = tryRecv(*consB);
        EXPECT_EQ(nullptr, stillThere) << "client-B already drained its own copy; A's unsubscribe is unrelated";

        sB.unsubscribe(topicB, subName);
    }
}

// Attempting to create a durable consumer on a Queue must throw.
TEST_F(DurableSubscriberTest, testDurableConsumerOnQueueThrows) {
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    ASSERT_NE(queue, nullptr);

    EXPECT_THROW(session.createDurableConsumer(queue, "bad-sub"), Poco::RuntimeException);
}

// After the active durable consumer acks all messages and reconnects, the
// subscription should be empty (no double-delivery).
TEST_F(DurableSubscriberTest, testAcknowledgedMessagesNotRedelivered) {
    const std::string subName = "sub-ack";
    {
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = s.createDurableConsumer(topic, subName);
        Producer::Ptr producer = s.createProducer(topic);

        producer->send(s.createTextMessage("ack-me", Message::PERSISTENT));

        auto r = tryRecv(*sub, 500000);
        ASSERT_NE(r, nullptr);
        sub->acknowledgeOn(*r);
        // Consumer goes offline here
    }

    {
        // Reconnect — no messages should be waiting
        Connection s_conn(*_exchange);
        Session &s = s_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr topic = s.createDestination(tiny_mq::destination::Topic, CurrentTestName);
        Consumer::Ptr sub = s.createDurableConsumer(topic, subName);
        ASSERT_NE(sub, nullptr);

        auto r = tryRecv(*sub);
        EXPECT_EQ(nullptr, r) << "Acknowledged messages must not be redelivered";

        s.unsubscribe(topic, subName);
    }
}
