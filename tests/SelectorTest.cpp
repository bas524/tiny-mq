//
// Created by Alexander Bychuk on 12.04.2026.
//

#include "SelectorTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TextMessage.h"
#include "Selector.h"
#include "TestHelper.h"

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::InvalidSelectorException;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Selector;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::TextMessage;

void SelectorTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void SelectorTest::TearDown() { _exchange.reset(); }

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Build a TextMessage pre-loaded with the standard set of selector test properties.
TextMessage makeTestMessage(Session &session) {
  TextMessage msg = session.createTextMessage("selector-test-body");

  // String properties
  msg.setStringProperty("name", "James");
  msg.setStringProperty("JMSType", "selector-test");
  msg.setStringProperty("quote", "'In God We Trust'");

  // Numeric properties
  msg.setIntProperty("rank", 123);
  msg.setLongProperty("bigRank", 1000000LL);
  msg.setDoubleProperty("score", 98.6);

  // Boolean properties
  msg.setBoolProperty("trueProp", true);
  msg.setBoolProperty("falseProp", false);

  // "dummy" is intentionally NOT set so IS NULL tests work.

  return msg;
}

/// Send msg on producer, commit if transacted, then recv from consumer.
/// Returns the received message (non-null means it passed the selector).
Message::Ptr sendRecv(Session &session, Producer &producer, Consumer &consumer, const TextMessage &msg,
                      int64_t timeoutUsec = 200000) {
  producer.send(msg);
  if (session.acknowledgeMode() == Session::AcknowledgeMode::SESSION_TRANSACTED) {
    session.commit();
  }
  return consumer.recv(timeoutUsec);
}

/// Helper: assert that a selector expression matches (or does not match) the standard test message.
void assertSelector(Session &session, Destination &destination, const std::string &selectorExpr, bool expected) {
  SCOPED_TRACE(selectorExpr);

  Consumer::Ptr consumer = session.createConsumer(destination, selectorExpr);
  ASSERT_NE(consumer, nullptr);

  Producer::Ptr producer = session.createProducer(destination);
  ASSERT_NE(producer, nullptr);

  TextMessage msg = makeTestMessage(session);
  Message::Ptr received = sendRecv(session, *producer, *consumer, msg);

  if (expected) {
    EXPECT_NE(received, nullptr) << "Selector should have matched but message was filtered out: " << selectorExpr;
  } else {
    EXPECT_EQ(received, nullptr) << "Selector should NOT have matched but message was delivered: " << selectorExpr;
  }

  session.deleteConsumer(consumer->id());
  session.deleteProducer(producer->id());
}

/// Helper: assert that parsing the selector expression throws InvalidSelectorException.
void assertInvalidSelector(const std::string &selectorExpr) {
  SCOPED_TRACE(selectorExpr);
  EXPECT_THROW(Selector::parse(selectorExpr), InvalidSelectorException)
      << "Expected InvalidSelectorException for: " << selectorExpr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parse / validation tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testInvalidSelectorSyntax) {
  assertInvalidSelector("=TEST 'test'");
  assertInvalidSelector("rank > AND name = 'James'");
  assertInvalidSelector("name NOT 'James'");
}

TEST_F(SelectorTest, testEmptySelectorMatchesAll) {
  auto sel = Selector::parse("");
  ASSERT_NE(sel, nullptr);

  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Consumer::Ptr consumer = session.createConsumer(destination, "");
  Producer::Ptr producer = session.createProducer(destination);

  TextMessage msg = makeTestMessage(session);
  producer->send(msg);
  Message::Ptr received = consumer->recv(200000);
  EXPECT_NE(received, nullptr);
}

// ---------------------------------------------------------------------------
// String comparison tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testStringEquality) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "name = 'James'", true);
  assertSelector(session, *destination, "name = 'Bob'", false);
  assertSelector(session, *destination, "name <> 'Bob'", true);
}

TEST_F(SelectorTest, testJMSTypeSelector) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "JMSType = 'selector-test'", true);
  assertSelector(session, *destination, "JMSType = 'other'", false);
}

// ---------------------------------------------------------------------------
// Numeric comparison tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testNumericComparisons) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "rank > 100", true);
  assertSelector(session, *destination, "rank > 200", false);
  assertSelector(session, *destination, "rank >= 123", true);
  assertSelector(session, *destination, "rank < 200", true);
  assertSelector(session, *destination, "rank <= 123", true);
  assertSelector(session, *destination, "rank = 123", true);
  assertSelector(session, *destination, "rank <> 123", false);
}

TEST_F(SelectorTest, testDoubleComparisons) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "score > 90.0", true);
  assertSelector(session, *destination, "score < 100.0", true);
  assertSelector(session, *destination, "score = 98.6", true);
}

// ---------------------------------------------------------------------------
// Boolean logic tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testBooleanLogic) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "trueProp", true);
  assertSelector(session, *destination, "falseProp", false);
  assertSelector(session, *destination, "trueProp AND trueProp", true);
  assertSelector(session, *destination, "trueProp AND falseProp", false);
  assertSelector(session, *destination, "trueProp OR falseProp", true);
  assertSelector(session, *destination, "falseProp OR falseProp", false);
  assertSelector(session, *destination, "(trueProp OR falseProp) AND trueProp", true);
  assertSelector(session, *destination, "NOT falseProp", true);
  assertSelector(session, *destination, "NOT trueProp", false);
}

// ---------------------------------------------------------------------------
// BETWEEN tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testBetween) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "rank between 100 and 150", true);
  assertSelector(session, *destination, "rank between 123 and 123", true);
  assertSelector(session, *destination, "rank between 200 and 300", false);
  assertSelector(session, *destination, "rank not between 200 and 300", true);
  assertSelector(session, *destination, "rank not between 100 and 150", false);
}

// ---------------------------------------------------------------------------
// IN tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testIn) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "name in ('James', 'Bob', 'Gromit')", true);
  assertSelector(session, *destination, "name in ('Alice', 'Bob')", false);
  assertSelector(session, *destination, "name not in ('Alice', 'Bob')", true);
  assertSelector(session, *destination, "rank in (100, 123, 200)", true);
  assertSelector(session, *destination, "rank in (100, 200, 300)", false);
}

// ---------------------------------------------------------------------------
// IS NULL tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testIsNull) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  // "dummy" property is never set → IS NULL should match
  assertSelector(session, *destination, "dummy is null", true);
  assertSelector(session, *destination, "dummy is not null", false);
  // "name" IS set → IS NOT NULL should match
  assertSelector(session, *destination, "name is not null", true);
  assertSelector(session, *destination, "name is null", false);
}

// ---------------------------------------------------------------------------
// LIKE tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testLike) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "name LIKE 'J%'", true);
  assertSelector(session, *destination, "name LIKE '%ames'", true);
  assertSelector(session, *destination, "name LIKE 'J_mes'", true);
  assertSelector(session, *destination, "name LIKE 'B%'", false);
  assertSelector(session, *destination, "name NOT LIKE 'B%'", true);
  // Escaped single-quote in LIKE pattern: '''In G_d We Trust''' → 'In G_d We Trust'
  // quote property value is: 'In God We Trust'
  assertSelector(session, *destination, "quote LIKE '''In G_d We Trust'''", true);
}

// ---------------------------------------------------------------------------
// Compound / mixed tests
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testCompoundSelectors) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  assertSelector(session, *destination, "name = 'James' AND rank > 100", true);
  assertSelector(session, *destination, "name = 'James' AND rank > 200", false);
  assertSelector(session, *destination, "name = 'Bob' OR rank = 123", true);
  assertSelector(session, *destination, "NOT (name = 'Bob') AND rank = 123", true);
}

// ---------------------------------------------------------------------------
// Transacted session selector test
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testSelectorWithTransactedSession) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);

  Consumer::Ptr consumer = session.createConsumer(destination, "rank > 100");
  ASSERT_NE(consumer, nullptr);

  Producer::Ptr producer = session.createProducer(destination);
  ASSERT_NE(producer, nullptr);

  TextMessage matching = makeTestMessage(session);  // rank = 123 > 100 → matches

  // Build non-matching message from scratch: Properties::setProperty uses emplace
  // which won't overwrite an existing key, so we can't reuse makeTestMessage here.
  TextMessage nonMatching = session.createTextMessage("non-matching-body");
  nonMatching.setIntProperty("rank", 50);  // 50 <= 100 → filtered

  producer->send(matching);
  producer->send(nonMatching);
  session.commit();

  // Only the matching message should be delivered
  Message::Ptr received = consumer->recv(500000);
  EXPECT_NE(received, nullptr) << "Matching message should have been received";

  // No more messages (non-matching was filtered)
  Message::Ptr extra = consumer->recv(200000);
  EXPECT_EQ(extra, nullptr) << "Non-matching message should have been filtered out";

  session.commit();
}

// ---------------------------------------------------------------------------
// Multiple consumers with different selectors on same topic
// ---------------------------------------------------------------------------

TEST_F(SelectorTest, testTopicMultipleSelectors) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Topic, CurrentTestName);

  Consumer::Ptr consumerJames = session.createConsumer(destination, "name = 'James'");
  Consumer::Ptr consumerBob = session.createConsumer(destination, "name = 'Bob'");
  Producer::Ptr producer = session.createProducer(destination);

  TextMessage msg = makeTestMessage(session);  // name = "James"
  producer->send(msg);

  // consumerJames should receive it
  Message::Ptr recvJames = consumerJames->recv(500000);
  EXPECT_NE(recvJames, nullptr) << "consumerJames should receive the message";

  // consumerBob should NOT receive it (filtered by selector)
  Message::Ptr recvBob = consumerBob->recv(200000);
  EXPECT_EQ(recvBob, nullptr) << "consumerBob should not receive the message (wrong name)";
}
