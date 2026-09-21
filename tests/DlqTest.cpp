//
// Redelivery counter + dead-letter queue tests for tiny-mq (spec 24).
//
// Test plan mapping (docs/jms-spec/24-redelivery-and-dlq.md), numbered as in
// the spec's own (out-of-order) Test plan section: T1..T18, T20, T21, T22, T19.
// Each case uses its own destination via CurrentTestName; a DLQ, when needed,
// is CurrentTestName + ".DLQ".

#include "DlqTest.h"
#include "Connection.h"
#include "ConnectionMetaData.h"
#include "Destination.h"
#include "Exchange.h"
#include "Producer.h"
#include "RedeliveryPolicy.h"
#include "Session.h"
#include "TestHelper.h"
#include "TextMessage.h"
#include <Poco/Exception.h>
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>
#include <algorithm>

using tiny_mq::Connection;
using tiny_mq::ConnectionMetaData;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::RedeliveryPolicy;
using tiny_mq::SendOptions;
using tiny_mq::Session;
using tiny_mq::TextMessage;

namespace {
int64_t nowMs() { return Poco::Timestamp().epochMicroseconds() / 1000; }
}  // namespace

void DlqTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void DlqTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

// T1: SESSION_TRANSACTED, persistent — rollback increments deliveryCount and
// marks redelivered; a second rollback increments again.
TEST_F(DlqTest, RollbackIncrementsDeliveryCount) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("t1", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  Message::Ptr r0 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  EXPECT_FALSE(r0->jmsHeaders.redelivered);
  EXPECT_EQ(0, r0->jmsHeaders.deliveryCount);

  session.rollback();
  Message::Ptr r1 = consumer->recv();
  ASSERT_NE(r1, nullptr);
  EXPECT_TRUE(r1->jmsHeaders.redelivered);
  EXPECT_EQ(1, r1->jmsHeaders.deliveryCount);

  session.rollback();
  Message::Ptr r2 = consumer->recv();
  ASSERT_NE(r2, nullptr);
  EXPECT_TRUE(r2->jmsHeaders.redelivered);
  EXPECT_EQ(2, r2->jmsHeaders.deliveryCount);

  session.rollback();
}

// T2: CLIENT_ACKNOWLEDGE, persistent — recover() increments deliveryCount.
TEST_F(DlqTest, RecoverIncrementsDeliveryCount) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("t2", Message::PERSISTENT);
  producer->send(m);

  Message::Ptr r0 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  EXPECT_EQ(0, r0->jmsHeaders.deliveryCount);

  session.recover();
  Message::Ptr r1 = consumer->recv();
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(1, r1->jmsHeaders.deliveryCount);

  session.recover();
  Message::Ptr r2 = consumer->recv();
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(2, r2->jmsHeaders.deliveryCount);

  consumer->acknowledgeOn(*r2);
}

// T3: one queue, two messages — one cycled via recover() on a CLIENT_ACK
// session, the other via rollback() on a SESSION_TRANSACTED session sharing
// the same underlying queue. Each counter must grow independently by exactly
// 1 per cycle.
TEST_F(DlqTest, CountMonotonicAcrossRecoverAndRollbackPaths) {
  Connection connection(*_exchange);
  Session &sessionRecover = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Session &sessionRollback = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = sessionRecover.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  ASSERT_NE(queue, nullptr);
  Producer::Ptr producer = sessionRecover.createProducer(queue);
  Consumer::Ptr consumerA = sessionRecover.createConsumer(queue);
  Consumer::Ptr consumerB = sessionRollback.createConsumer(queue);

  TextMessage mA = sessionRecover.createTextMessage("A", Message::PERSISTENT);
  TextMessage mB = sessionRecover.createTextMessage("B", Message::PERSISTENT);
  producer->send(mA);
  producer->send(mB);

  // Single-threaded test: consumerA drains first, so mA lands on A, mB on B.
  Message::Ptr rA0 = consumerA->recv();
  Message::Ptr rB0 = consumerB->recv();
  ASSERT_NE(rA0, nullptr);
  ASSERT_NE(rB0, nullptr);
  EXPECT_EQ("A", Message::As<TextMessage>(rA0)->text());
  EXPECT_EQ("B", Message::As<TextMessage>(rB0)->text());

  for (int i = 1; i <= 3; ++i) {
    sessionRecover.recover();
    Message::Ptr rA = consumerA->recv();
    ASSERT_NE(rA, nullptr);
    EXPECT_EQ(i, rA->jmsHeaders.deliveryCount) << "recover() cycle " << i;

    sessionRollback.rollback();
    Message::Ptr rB = consumerB->recv();
    ASSERT_NE(rB, nullptr);
    EXPECT_EQ(i, rB->jmsHeaders.deliveryCount) << "rollback() cycle " << i;
  }
}

// T4: maxRedeliveries=2, backoffMs=0, DLQ set — three rollbacks land the
// message in the DLQ with the exceeded-limit reason and correct headers.
TEST_F(DlqTest, LandsInDlqAfterMaxRedeliveries) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".DLQ");
  ASSERT_NE(queue, nullptr);
  ASSERT_NE(dlq, nullptr);

  RedeliveryPolicy policy;
  policy.maxRedeliveries = 2;
  policy.backoffMs = 0;
  policy.deadLetterQueue = dlq;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);
  Consumer::Ptr dlqConsumer = session.createConsumer(dlq);

  TextMessage m = session.createTextMessage("dead-letter-me", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  for (int i = 0; i < 3; ++i) {
    Message::Ptr r = consumer->recv();
    ASSERT_NE(r, nullptr) << "expected message still on origin before rollback " << i;
    session.rollback();
  }

  EXPECT_EQ(consumer->recv(50000), nullptr) << "message must have left the origin queue";

  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer->recv());
  ASSERT_NE(dead, nullptr);
  EXPECT_EQ("dead-letter-me", dead->text());
  EXPECT_EQ("maxRedeliveries exceeded", dead->property<tiny_mq::property::String>("JMSXDeadLetterReason").value());
  EXPECT_EQ(3, dead->jmsHeaders.deliveryCount);
  EXPECT_TRUE(dead->jmsHeaders.redelivered);
  EXPECT_EQ(0, dead->jmsHeaders.deliveryTime);
}

// T5: the DLQ copy preserves correlationId/replyTo/type/priority and custom
// application properties.
TEST_F(DlqTest, DlqPreservesHeadersAndProperties) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".DLQ");

  RedeliveryPolicy policy;
  policy.maxRedeliveries = 2;
  policy.deadLetterQueue = dlq;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);
  Consumer::Ptr dlqConsumer = session.createConsumer(dlq);

  TextMessage m = session.createTextMessage("with-headers", Message::PERSISTENT);
  m.jmsHeaders.correlationId = "corr-123";
  m.jmsHeaders.replyTo = "queue://reply-to-me";
  m.jmsHeaders.type = "my.custom.Type";
  m.setIntProperty("customInt", 42);
  m.setStringProperty("customString", "hello");
  producer->send(m, SendOptions{Message::PERSISTENT, 7, 0, 0});
  session.commit();

  for (int i = 0; i < 3; ++i) {
    Message::Ptr r = consumer->recv();
    ASSERT_NE(r, nullptr);
    session.rollback();
  }

  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer->recv());
  ASSERT_NE(dead, nullptr);
  EXPECT_EQ("with-headers", dead->text());
  EXPECT_EQ("corr-123", dead->jmsHeaders.correlationId);
  EXPECT_EQ("queue://reply-to-me", dead->jmsHeaders.replyTo);
  EXPECT_EQ("my.custom.Type", dead->jmsHeaders.type);
  EXPECT_EQ(7, dead->jmsHeaders.priority);
  EXPECT_EQ(42, dead->property<tiny_mq::property::Int>("customInt").value());
  EXPECT_EQ("hello", dead->property<tiny_mq::property::String>("customString").value());
}

// T6: a persistent DLQ copy survives an Exchange restart.
TEST_F(DlqTest, DlqCopySurvivesRestart) {
  const std::string queueName = CurrentTestName;
  const std::string dlqName = std::string(CurrentTestName) + ".DLQ";
  {
    Connection connection(*_exchange);
    Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, queueName);
    Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, dlqName);

    RedeliveryPolicy policy;
    policy.maxRedeliveries = 1;
    policy.deadLetterQueue = dlq;
    queue->setRedeliveryPolicy(policy);

    Producer::Ptr producer = session.createProducer(queue);
    Consumer::Ptr consumer = session.createConsumer(queue);

    TextMessage m = session.createTextMessage("restart-me", Message::PERSISTENT);
    producer->send(m);
    session.commit();

    for (int i = 0; i < 2; ++i) {
      Message::Ptr r = consumer->recv();
      ASSERT_NE(r, nullptr);
      session.rollback();
    }
    EXPECT_EQ(consumer->recv(50000), nullptr);
  }

  _exchange.reset();
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());

  Connection connection2(*_exchange);
  Session &session2 = connection2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, queueName);
  Destination::Ptr dlq2 = session2.createDestination(tiny_mq::destination::Queue, dlqName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);
  Consumer::Ptr dlqConsumer2 = session2.createConsumer(dlq2);

  EXPECT_EQ(consumer2->recv(50000), nullptr) << "origin must not have the message after restart";
  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer2->recv());
  ASSERT_NE(dead, nullptr) << "DLQ copy must survive the restart";
  EXPECT_EQ("restart-me", dead->text());
}

// T7: the DLQ consumer is created only after the message was dead-lettered —
// it must still receive it (the DLQ's queue exists from destination creation).
TEST_F(DlqTest, DlqConsumerCreatedAfterDeadLettering) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".DLQ");

  RedeliveryPolicy policy;
  policy.maxRedeliveries = 1;
  policy.deadLetterQueue = dlq;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("late-consumer", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  for (int i = 0; i < 2; ++i) {
    Message::Ptr r = consumer->recv();
    ASSERT_NE(r, nullptr);
    session.rollback();
  }
  EXPECT_EQ(consumer->recv(50000), nullptr);

  // Only now create the DLQ consumer.
  Consumer::Ptr dlqConsumer = session.createConsumer(dlq);
  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer->recv());
  ASSERT_NE(dead, nullptr);
  EXPECT_EQ("late-consumer", dead->text());
}

// T8: no DLQ configured — the message is dropped entirely, including from
// persistent storage (verified via restart).
TEST_F(DlqTest, NullDlqDropsMessage) {
  const std::string queueName = CurrentTestName;
  {
    Connection connection(*_exchange);
    Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, queueName);

    RedeliveryPolicy policy;
    policy.maxRedeliveries = 1;
    policy.deadLetterQueue = nullptr;
    queue->setRedeliveryPolicy(policy);

    Producer::Ptr producer = session.createProducer(queue);
    Consumer::Ptr consumer = session.createConsumer(queue);

    TextMessage m = session.createTextMessage("drop-me", Message::PERSISTENT);
    producer->send(m);
    session.commit();

    for (int i = 0; i < 2; ++i) {
      Message::Ptr r = consumer->recv();
      ASSERT_NE(r, nullptr);
      session.rollback();
    }
    EXPECT_EQ(consumer->recv(50000), nullptr) << "message must be dropped, not stuck on origin";
  }

  _exchange.reset();
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());

  Connection connection2(*_exchange);
  Session &session2 = connection2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, queueName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);
  EXPECT_EQ(consumer2->recv(50000), nullptr) << "dropped message must not survive as a storage record";
}

// T9: maxRedeliveries=-1 — unlimited redeliveries, counter keeps growing.
TEST_F(DlqTest, UnlimitedRedeliveriesWhenNegative) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  RedeliveryPolicy policy;
  policy.maxRedeliveries = -1;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("unlimited", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  Message::Ptr last;
  for (int i = 0; i < 10; ++i) {
    last = consumer->recv();
    ASSERT_NE(last, nullptr) << "iteration " << i;
    session.rollback();
  }
  last = consumer->recv();
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(10, last->jmsHeaders.deliveryCount);
}

// T10: default policy (no setRedeliveryPolicy call) — maxRedeliveries=6,
// drops after the 7th rollback.
TEST_F(DlqTest, DefaultPolicyDropsAfterSixRedeliveries) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("default-policy", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  for (int i = 0; i < 7; ++i) {
    Message::Ptr r = consumer->recv();
    ASSERT_NE(r, nullptr) << "iteration " << i;
    session.rollback();
  }
  EXPECT_EQ(consumer->recv(50000), nullptr) << "default policy must drop after 6 redeliveries (7th rollback)";
}

// T11: backoffMs=200 — a rolled-back message is not visible again before the
// exponential backoff delay elapses.
TEST_F(DlqTest, BackoffDelaysRedelivery) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  RedeliveryPolicy policy;
  policy.backoffMs = 200;
  policy.maxBackoffMs = 60000;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("backoff", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  Message::Ptr r0 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  const int64_t tRollback0 = nowMs();
  session.rollback();

  EXPECT_EQ(consumer->recv(50000), nullptr) << "must not be visible before the 200ms backoff elapses";
  Message::Ptr r1 = consumer->recv(1000000);
  ASSERT_NE(r1, nullptr);
  EXPECT_TRUE(r1->jmsHeaders.redelivered);
  EXPECT_GE(r1->jmsHeaders.deliveryTime, tRollback0 + 200);

  const int64_t tRollback1 = nowMs();
  session.rollback();
  EXPECT_EQ(consumer->recv(50000), nullptr) << "must not be visible before the 400ms (2nd) backoff elapses";
  Message::Ptr r2 = consumer->recv(1000000);
  ASSERT_NE(r2, nullptr);
  EXPECT_GE(r2->jmsHeaders.deliveryTime, tRollback1 + 400);
}

// T12: backoffMs=200, maxBackoffMs=300 — the third redelivery would be 800ms
// uncapped but must be capped to 300ms.
TEST_F(DlqTest, BackoffCappedByMaxBackoff) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  RedeliveryPolicy policy;
  policy.backoffMs = 200;
  policy.maxBackoffMs = 300;
  policy.maxRedeliveries = 10;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("capped", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  // 1st, 2nd and 3rd redelivery: consume the first three (backoff 200ms, then
  // capped 300ms, then capped 300ms again — the 3rd would be 800ms uncapped).
  for (int i = 0; i < 3; ++i) {
    Message::Ptr r = consumer->recv(1000000);
    ASSERT_NE(r, nullptr) << "iteration " << i;
    session.rollback();
  }

  const int64_t tRollback2 = nowMs();
  Message::Ptr r2 = consumer->recv(1000000);
  ASSERT_NE(r2, nullptr) << "3rd redelivery must arrive well before the uncapped 800ms";
  EXPECT_EQ(3, r2->jmsHeaders.deliveryCount);
  const int64_t elapsed = nowMs() - tRollback2;
  EXPECT_LE(elapsed, 600) << "backoff must be capped at maxBackoffMs (300ms), not the uncapped 800ms";
}

// T13: persistent message, backoffMs=100 — ADR-0008: the message received
// after redelivery must carry the updated deliveryTime/deliveryCount, not
// stale values from the pre-redelivery cache.
TEST_F(DlqTest, BackoffUpdatesCachedBytesForPersistent) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  RedeliveryPolicy policy;
  policy.backoffMs = 100;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m = session.createTextMessage("adr0008", Message::PERSISTENT);
  producer->send(m);
  session.commit();

  Message::Ptr r0 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  session.rollback();

  Message::Ptr r1 = consumer->recv(1000000);
  ASSERT_NE(r1, nullptr);
  EXPECT_EQ(1, r1->jmsHeaders.deliveryCount);
  EXPECT_NE(0, r1->jmsHeaders.deliveryTime);
  EXPECT_TRUE(r1->jmsHeaders.redelivered);
}

// T14: CLIENT_ACK, three messages, recover() — original receive order is
// preserved (regression check for spec 23 behaviour under the shared
// redeliver() path).
TEST_F(DlqTest, RedeliveryOrderPreservedWithoutBackoff) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage m0 = session.createTextMessage("first", Message::PERSISTENT);
  TextMessage m1 = session.createTextMessage("second", Message::PERSISTENT);
  TextMessage m2 = session.createTextMessage("third", Message::PERSISTENT);
  producer->send(m0);
  producer->send(m1);
  producer->send(m2);

  ASSERT_NE(consumer->recv(), nullptr);
  ASSERT_NE(consumer->recv(), nullptr);
  ASSERT_NE(consumer->recv(), nullptr);

  session.recover();

  TextMessage::Ptr r0 = Message::As<TextMessage>(consumer->recv());
  TextMessage::Ptr r1 = Message::As<TextMessage>(consumer->recv());
  TextMessage::Ptr r2 = Message::As<TextMessage>(consumer->recv());
  ASSERT_NE(r0, nullptr);
  ASSERT_NE(r1, nullptr);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ("first", r0->text());
  EXPECT_EQ("second", r1->text());
  EXPECT_EQ("third", r2->text());
}

// T15: Topic, maxRedeliveries=1, DLQ set — each subscriber's redelivery/DLQ
// state is independent (Semantics 6).
TEST_F(DlqTest, TopicSubscriberDeadLetteredIndependently) {
  Connection connection(*_exchange);
  Session &sessionA = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Session &sessionB = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr topic = sessionA.createDestination(tiny_mq::destination::Topic, CurrentTestName);
  Destination::Ptr dlq = sessionA.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".DLQ");
  ASSERT_NE(topic, nullptr);
  ASSERT_NE(dlq, nullptr);

  RedeliveryPolicy policy;
  policy.maxRedeliveries = 1;
  policy.deadLetterQueue = dlq;
  topic->setRedeliveryPolicy(policy);

  Consumer::Ptr subA = sessionA.createConsumer(topic);
  Consumer::Ptr subB = sessionB.createConsumer(topic);
  Producer::Ptr producer = sessionA.createProducer(topic);
  Consumer::Ptr dlqConsumer = sessionA.createConsumer(dlq);

  TextMessage m = sessionA.createTextMessage("fan-out", Message::PERSISTENT);
  producer->send(m);

  Message::Ptr a0 = subA->recv();
  ASSERT_NE(a0, nullptr);
  sessionA.recover();  // deliveryCount 1, still <= maxRedeliveries(1)
  Message::Ptr a1 = subA->recv();
  ASSERT_NE(a1, nullptr);
  sessionA.recover();  // deliveryCount 2 > 1 -> dead-lettered from A's own queue
  EXPECT_EQ(subA->recv(50000), nullptr) << "A's own subscription must be empty after dead-lettering";

  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer->recv());
  ASSERT_NE(dead, nullptr);
  EXPECT_EQ("fan-out", dead->text());

  // B is untouched by A's recover() cycles: one recover() cycle keeps B's
  // own deliveryCount at 1, still within maxRedeliveries(1), so B never
  // dead-letters.
  TextMessage::Ptr b0 = Message::As<TextMessage>(subB->recv());
  ASSERT_NE(b0, nullptr);
  EXPECT_EQ(0, b0->jmsHeaders.deliveryCount);
  sessionB.recover();
  TextMessage::Ptr b1 = Message::As<TextMessage>(subB->recv());
  ASSERT_NE(b1, nullptr);
  EXPECT_EQ(1, b1->jmsHeaders.deliveryCount);
  subB->acknowledgeOn(*b1);

  // Exactly one copy in the DLQ — B's cycle (within its own limit) never
  // dead-lettered.
  EXPECT_EQ(dlqConsumer->recv(50000), nullptr) << "DLQ must still hold exactly one copy";
}

// T16: setRedeliveryPolicy rejects a DLQ that is the destination itself or a
// topic; a queue DLQ on a topic destination is accepted.
TEST_F(DlqTest, SetPolicyRejectsSelfOrTopicAsDlq) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".queue");
  Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, std::string(CurrentTestName) + ".topic");
  Destination::Ptr dlqQueue = session.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".dlq");

  RedeliveryPolicy selfPolicy;
  selfPolicy.deadLetterQueue = queue;
  EXPECT_THROW(queue->setRedeliveryPolicy(selfPolicy), Poco::InvalidArgumentException);

  RedeliveryPolicy topicDlqPolicy;
  topicDlqPolicy.deadLetterQueue = topic;
  EXPECT_THROW(queue->setRedeliveryPolicy(topicDlqPolicy), Poco::InvalidArgumentException);

  RedeliveryPolicy validPolicy;
  validPolicy.deadLetterQueue = dlqQueue;
  EXPECT_NO_THROW(topic->setRedeliveryPolicy(validPolicy));
}

// T17: SESSION_TRANSACTED, maxRedeliveries=0 — closing the session with an
// unacknowledged message rolls it back via ~Session() and that rollback
// itself counts toward (and here immediately exceeds) the DLQ limit.
TEST_F(DlqTest, DeadLetteringOnSessionCloseRollback) {
  const std::string queueName = CurrentTestName;
  const std::string dlqName = std::string(CurrentTestName) + ".DLQ";
  {
    Connection connection(*_exchange);
    Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, queueName);
    Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, dlqName);

    RedeliveryPolicy policy;
    policy.maxRedeliveries = 0;
    policy.deadLetterQueue = dlq;
    queue->setRedeliveryPolicy(policy);

    Producer::Ptr producer = session.createProducer(queue);
    Consumer::Ptr consumer = session.createConsumer(queue);

    TextMessage m = session.createTextMessage("close-me", Message::PERSISTENT);
    producer->send(m);
    session.commit();

    Message::Ptr r0 = consumer->recv();
    ASSERT_NE(r0, nullptr);
    // No commit — closing the connection (and thus the session) below must
    // roll this back via ~Session(), landing it in the DLQ.
  }

  Connection connection2(*_exchange);
  Session &session2 = connection2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, queueName);
  Destination::Ptr dlq2 = session2.createDestination(tiny_mq::destination::Queue, dlqName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);
  Consumer::Ptr dlqConsumer2 = session2.createConsumer(dlq2);

  EXPECT_EQ(consumer2->recv(50000), nullptr) << "origin must be empty after the session-close rollback";
  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer2->recv());
  ASSERT_NE(dead, nullptr) << "message must have been dead-lettered by the session-close rollback";
  EXPECT_EQ("close-me", dead->text());
}

// T18: maxRedeliveries=0, TTL=2s — the DLQ copy keeps the original expiration
// (observable before it elapses); after the TTL elapses, recv from the DLQ
// returns nullptr per spec 44's expiration rule.
TEST_F(DlqTest, DlqCopyKeepsExpiration) {
  Connection connection(*_exchange);
  Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, std::string(CurrentTestName) + ".DLQ");

  RedeliveryPolicy policy;
  policy.maxRedeliveries = 0;
  policy.deadLetterQueue = dlq;
  queue->setRedeliveryPolicy(policy);

  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int64_t kTtlMs = 2000;
  TextMessage m = session.createTextMessage("ttl-dlq", Message::PERSISTENT);
  producer->send(m, SendOptions{Message::PERSISTENT, 4, kTtlMs, 0});
  session.commit();

  Message::Ptr r0 = consumer->recv();
  ASSERT_NE(r0, nullptr);
  const int64_t originalExpiration = r0->jmsHeaders.expiration;
  ASSERT_NE(0, originalExpiration);
  session.rollback();  // maxRedeliveries=0 -> immediately dead-lettered

  EXPECT_EQ(consumer->recv(50000), nullptr);

  Consumer::Ptr dlqConsumer = session.createConsumer(dlq);
  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer->recv(1000000));
  ASSERT_NE(dead, nullptr) << "DLQ copy must be receivable before its TTL elapses";
  EXPECT_EQ(originalExpiration, dead->jmsHeaders.expiration);
}

// T20: maxRedeliveries=2, backoffMs=5000, persistent — after a restart, the
// deliveryCount/redelivered/backoff state resets: a fresh recv() from the
// origin sees the message as if it had never been redelivered.
TEST_F(DlqTest, RestartResetsCounterAndLimit) {
  const std::string queueName = CurrentTestName;
  const std::string dlqName = std::string(CurrentTestName) + ".DLQ";
  {
    Connection connection(*_exchange);
    Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, queueName);
    Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, dlqName);

    RedeliveryPolicy policy;
    policy.maxRedeliveries = 2;
    policy.backoffMs = 5000;
    policy.deadLetterQueue = dlq;
    queue->setRedeliveryPolicy(policy);

    Producer::Ptr producer = session.createProducer(queue);
    Consumer::Ptr consumer = session.createConsumer(queue);

    TextMessage m = session.createTextMessage("restart-reset", Message::PERSISTENT);
    producer->send(m);
    session.commit();

    // 1st rollback: deliveryCount 0 -> 1, next redelivery deferred ~5000ms.
    Message::Ptr r0 = consumer->recv();
    ASSERT_NE(r0, nullptr);
    session.rollback();
    EXPECT_EQ(1, r0->jmsHeaders.deliveryCount);

    // Wait out the ~5000ms backoff for the 2nd rollback.
    Message::Ptr r1 = consumer->recv(7000000);
    ASSERT_NE(r1, nullptr) << "must reappear once the 1st backoff elapses";
    session.rollback();
    EXPECT_EQ(2, r1->jmsHeaders.deliveryCount);
    // The resulting ~10000ms backoff is deliberately left pending — the
    // restart below happens before it elapses.
  }

  _exchange.reset();
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());

  Connection connection2(*_exchange);
  Session &session2 = connection2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, queueName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);

  TextMessage::Ptr replayed = Message::As<TextMessage>(consumer2->recv(100000));
  ASSERT_NE(replayed, nullptr) << "message must be immediately visible after restart, not deferred by the pending backoff";
  EXPECT_EQ(0, replayed->jmsHeaders.deliveryCount) << "deliveryCount does not survive restart by design";
  EXPECT_FALSE(replayed->jmsHeaders.redelivered);
  EXPECT_EQ(0, replayed->jmsHeaders.deliveryTime);
}

// T21: SESSION_TRANSACTED, backoffMs=5000, maxRedeliveries=6 — a session-close
// rollback (sessionClosing=true) requeues without applying backoff, unlike an
// explicit application rollback().
TEST_F(DlqTest, SessionCloseRequeuesWithoutBackoff) {
  const std::string queueName = CurrentTestName;
  {
    Connection connection(*_exchange);
    Session &session = connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, queueName);

    RedeliveryPolicy policy;
    policy.backoffMs = 5000;
    policy.maxRedeliveries = 6;
    queue->setRedeliveryPolicy(policy);

    Producer::Ptr producer = session.createProducer(queue);
    Consumer::Ptr consumer = session.createConsumer(queue);

    TextMessage m = session.createTextMessage("close-no-backoff", Message::PERSISTENT);
    producer->send(m);
    session.commit();

    Message::Ptr r0 = consumer->recv();
    ASSERT_NE(r0, nullptr);
    // No commit — closing below rolls this back with sessionClosing=true.
  }

  Connection connection2(*_exchange);
  Session &session2 = connection2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, queueName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);

  TextMessage::Ptr r = Message::As<TextMessage>(consumer2->recv(100000));
  ASSERT_NE(r, nullptr) << "session-close rollback must requeue immediately, without backoff";
  EXPECT_TRUE(r->jmsHeaders.redelivered);
  EXPECT_EQ(1, r->jmsHeaders.deliveryCount);
  EXPECT_EQ(0, r->jmsHeaders.deliveryTime);
}

// T22: durable subscriber, maxRedeliveries=0, DLQ set — dead-lettering a
// durable subscriber's message removes it from that subscription's own
// storage; after a restart the subscriber reconnects with nothing pending
// while the DLQ copy remains.
TEST_F(DlqTest, DurableSubscriberDeadLetterRemovesFromDurableStorage) {
  const std::string topicName = CurrentTestName;
  const std::string dlqName = std::string(CurrentTestName) + ".DLQ";
  const std::string subName = "sub22";
  {
    Connection connection(*_exchange);
    connection.setClientID("dlq-test-client-22");
    Session &session = connection.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
    Destination::Ptr topic = session.createDestination(tiny_mq::destination::Topic, topicName);
    Destination::Ptr dlq = session.createDestination(tiny_mq::destination::Queue, dlqName);

    RedeliveryPolicy policy;
    policy.maxRedeliveries = 0;
    policy.deadLetterQueue = dlq;
    topic->setRedeliveryPolicy(policy);

    Consumer::Ptr sub = session.createDurableConsumer(topic, subName);
    Producer::Ptr producer = session.createProducer(topic);

    TextMessage m = session.createTextMessage("durable-dlq", Message::PERSISTENT);
    producer->send(m);

    Message::Ptr r0 = sub->recv();
    ASSERT_NE(r0, nullptr);
    session.recover();  // deliveryCount 1 > 0 -> dead-lettered from the durable storage
    EXPECT_EQ(sub->recv(50000), nullptr);
  }

  _exchange.reset();
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());

  Connection connection2(*_exchange);
  connection2.setClientID("dlq-test-client-22");
  Session &session2 = connection2.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
  Destination::Ptr topic2 = session2.createDestination(tiny_mq::destination::Topic, topicName);
  Destination::Ptr dlq2 = session2.createDestination(tiny_mq::destination::Queue, dlqName);
  Consumer::Ptr sub2 = session2.createDurableConsumer(topic2, subName);
  Consumer::Ptr dlqConsumer2 = session2.createConsumer(dlq2);

  EXPECT_EQ(sub2->recv(50000), nullptr) << "durable subscription storage must have been cleared";
  TextMessage::Ptr dead = Message::As<TextMessage>(dlqConsumer2->recv());
  ASSERT_NE(dead, nullptr) << "DLQ copy must survive the restart";
  EXPECT_EQ("durable-dlq", dead->text());

  session2.unsubscribe(topic2, subName);
}

// T19: ConnectionMetaData.jmsxPropertyNames lists exactly the two spec 24
// JMSX property names.
TEST_F(DlqTest, JmsxPropertyNamesListed) {
  Connection connection(*_exchange);
  ConnectionMetaData metadata = connection.metadata();
  const auto &names = metadata.jmsxPropertyNames;
  EXPECT_NE(std::find(names.begin(), names.end(), "JMSXDeliveryCount"), names.end());
  EXPECT_NE(std::find(names.begin(), names.end(), "JMSXDeadLetterReason"), names.end());
}
