//
// DeliveryDelay tests for tiny-mq (JMS 2.0 § 7.8 — spec 13).
//
// Covers exactly the 3 items in docs/jms-spec/13-delivery-delay.md's Test plan:
//   1. a delayed message is invisible to consumers until its delay elapses;
//   2. a persistent delayed message survives an Exchange restart, still
//      respecting its original (remaining) delay;
//   3. for a transactional send the delay clock starts at commit (not send),
//      and rollback discards the message entirely.

#include "DeliveryDelayTest.h"
#include "Connection.h"
#include "DeliveryScheduler.h"
#include "Exchange.h"
#include "Producer.h"
#include "Session.h"
#include "TextMessage.h"
#include "TestHelper.h"
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>
#include <atomic>
#include <chrono>
#include <limits>
#include <thread>

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::SendOptions;
using tiny_mq::Session;
using tiny_mq::TextMessage;

void DeliveryDelayTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void DeliveryDelayTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

// Test plan #1: a message sent with a delivery delay must not be visible to a
// consumer until the delay has elapsed, then must be delivered.
TEST_F(DeliveryDelayTest, testDelayedMessageInvisibleUntilDue) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int64_t kDelayMs = 400;
  TextMessage delayed = session.createTextMessage("delayed");
  producer->send(delayed, SendOptions{Message::NOT_PERSISTENT, 4, 0, kDelayMs});

  // Not yet due: a short recv must see nothing.
  EXPECT_EQ(consumer->recv(50000), nullptr)
      << "message must be invisible before its delivery time";

  // Wait past the delay with margin, then it must arrive.
  auto received = Message::As<TextMessage>(consumer->recv(500000));
  ASSERT_NE(received, nullptr) << "delayed message must be delivered once due";
  EXPECT_EQ("delayed", received->text());
}

// Test plan #2: a persistent delayed message must survive an Exchange restart
// and still respect its (remaining) delivery delay after replay.
TEST_F(DeliveryDelayTest, testPersistentDelayedMessageSurvivesRestart) {
  const std::string destName = std::string(CurrentTestName) + "_queue";
  const std::string dir = CurrentTestSuiteStorageDir() + "/restart-delay-test";
  constexpr int64_t kDelayMs = 500;

  Poco::Timestamp sendTime;
  {
    tiny_mq::Exchange ex(dir);
    Connection conn(ex);
    Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, destName);
    Producer::Ptr producer = session.createProducer(queue);
    TextMessage delayed = session.createTextMessage("persisted-delayed", Message::PERSISTENT);
    producer->send(delayed, SendOptions{Message::PERSISTENT, 4, 0, kDelayMs});
    // Exchange goes out of scope here -> destructor runs, simulating a restart.
  }

  tiny_mq::Exchange ex2(dir);
  Connection conn2(ex2);
  Session &session2 = conn2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, destName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);

  // Only assert invisibility if the restart itself didn't already burn through
  // the whole delay window (keeps the test robust on a slow CI machine).
  const int64_t elapsedMs = sendTime.elapsed() / 1000;
  if (elapsedMs < kDelayMs) {
    EXPECT_EQ(consumer2->recv(50000), nullptr)
        << "delayed persistent message must stay invisible across restart until its delivery time";
  }

  auto received = Message::As<TextMessage>(consumer2->recv(2000000));
  ASSERT_NE(received, nullptr) << "persistent delayed message must eventually be delivered after restart";
  EXPECT_EQ("persisted-delayed", received->text());
}

// Test plan #3: for a transactional send, the delay clock starts at commit
// (not send), and a rollback discards the delayed message entirely.
TEST_F(DeliveryDelayTest, testTransactionalCommitStartsClockRollbackDiscards) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int64_t kDelayMs = 400;

  // --- rollback discards: a rolled-back delayed send must never be delivered,
  // even after waiting past what would have been its delay window.
  TextMessage rolledBack = session.createTextMessage("rolled-back");
  producer->send(rolledBack, SendOptions{Message::NOT_PERSISTENT, 4, 0, kDelayMs});
  session.rollback();
  Poco::Thread::sleep(static_cast<long>(kDelayMs + 150));
  EXPECT_EQ(consumer->recv(50000), nullptr)
      << "a rolled-back delayed message must never be delivered";

  // --- commit starts the clock, not send: elapse well past kDelayMs *before*
  // committing. If the clock had started at send(), the message would already
  // be due by the time commit() runs; it must not be.
  TextMessage delayed = session.createTextMessage("committed-delayed");
  producer->send(delayed, SendOptions{Message::NOT_PERSISTENT, 4, 0, kDelayMs});
  Poco::Thread::sleep(static_cast<long>(kDelayMs + 150));
  session.commit();
  EXPECT_EQ(consumer->recv(50000), nullptr)
      << "the delay clock starts at commit, not send — message must still be pending right after commit";

  auto received = Message::As<TextMessage>(consumer->recv(500000));
  ASSERT_NE(received, nullptr) << "message must be delivered once the commit-time delay elapses";
  EXPECT_EQ("committed-delayed", received->text());
}

// Round-2 fix: DeliveryScheduler::enqueueOrSchedule must not start a new worker
// thread once stop() has already run — otherwise a later stop() call (e.g. from
// the destructor's safety net) returns via its "already stopped" fast path
// without joining that thread, and ~std::thread on a still-joinable thread
// calls std::terminate (a crash no try/catch can intercept — ADR-0006's exact
// failure mode). Reproduces the scenario the review flagged: stop() called
// before any delayed message was ever scheduled (_threadStarted == false),
// then a delayed enqueueOrSchedule arrives afterwards.
TEST_F(DeliveryDelayTest, testSchedulerDeliversImmediatelyInsteadOfStartingThreadAfterStop) {
  auto queue = std::make_shared<QueueT>();
  tiny_mq::DeliveryScheduler scheduler;
  scheduler.stop();  // stopped before ever scheduling anything: _threadStarted stays false

  auto msg = std::make_shared<TextMessage>();
  msg->jmsHeaders.deliveryTime = (Poco::Timestamp().epochMicroseconds() / 1000) + 60000;  // 60s out
  scheduler.enqueueOrSchedule(queue, msg);  // must NOT spin up a worker thread

  QueueT::consumer_token_t token(*queue);
  tiny_mq::Message::Ptr received;
  ASSERT_TRUE(queue->wait_dequeue_timed(token, received, 50000))
      << "after stop(), enqueueOrSchedule must deliver immediately instead of "
         "silently dropping the message or (pre-fix) risking std::terminate";
  EXPECT_EQ(received, msg);
  // scheduler's destructor runs here and calls stop() again (idempotent safety
  // net) — must not crash / must not hang joining a thread that was never
  // started (or, pre-fix, one that leaked past the first stop()).
}

// Round-2 fix (B2): a persistent, transactional, delayed message must arrive
// with the real (commit-resolved) JMSDeliveryTime, not the pre-commit 0.
// Producer::commit() patches the deliveryTime in two independently-cached
// copies of the serialized bytes: the TransactionBuffer's buffered copy (which
// this test's *storage* ends up holding) and the Message's own
// _cachedStorageBytes (which Consumer::recv()'s fast path reads instead of
// storage, since this is the same process). This test exercises exactly that
// fast path — the one the missing patch broke — and also, in passing,
// exercises TransactionBuffer::patchDeliveryTime's offset-43 memcpy for real
// (previously that code path ran zero times across the whole suite because no
// test combined persistent + transactional + delayed).
TEST_F(DeliveryDelayTest, testPersistentTransactionalDelayedPreservesDeliveryTimeOnReceipt) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int64_t kDelayMs = 400;
  TextMessage delayed = session.createTextMessage("persisted-transactional-delayed", Message::PERSISTENT);
  producer->send(delayed, SendOptions{Message::PERSISTENT, 4, 0, kDelayMs});

  Poco::Timestamp commitTime;
  session.commit();

  EXPECT_EQ(consumer->recv(50000), nullptr)
      << "persistent transactional delayed message must stay invisible until its commit-resolved delivery time";

  auto received = Message::As<TextMessage>(consumer->recv(500000));
  ASSERT_NE(received, nullptr) << "delayed message must be delivered once due";
  EXPECT_EQ("persisted-transactional-delayed", received->text());

  ASSERT_NE(received->jmsHeaders.deliveryTime, 0)
      << "JMSDeliveryTime must reflect the real commit-resolved delivery time, not the "
         "pre-commit 0 — spec 13 review B2";
  const int64_t expectedDeliveryTimeMs = commitTime.epochMicroseconds() / 1000 + kDelayMs;
  EXPECT_NEAR(static_cast<double>(received->jmsHeaders.deliveryTime),
              static_cast<double>(expectedDeliveryTimeMs), 3000.0)
      << "JMSDeliveryTime must match the commit-time-resolved value (offset-43 patch)";
}

// Round-2 fix (B2, continued): the same combination must also come back
// correct after a restart, i.e. once the *only* surviving copy of the
// deliveryTime is whatever TransactionBuffer::patchDeliveryTime wrote into the
// bytes handed to storage (Consumer's own in-process cache doesn't survive a
// restart; a fresh Message shell is built straight from storage bytes).
TEST_F(DeliveryDelayTest, testPersistentTransactionalDelayedPreservesDeliveryTimeAcrossRestart) {
  const std::string destName = std::string(CurrentTestName) + "_queue";
  const std::string dir = CurrentTestSuiteStorageDir() + "/restart-transactional-delay-test";
  constexpr int64_t kDelayMs = 500;

  Poco::Timestamp commitTime;
  {
    tiny_mq::Exchange ex(dir);
    Connection conn(ex);
    Session &session = conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, destName);
    Producer::Ptr producer = session.createProducer(queue);
    TextMessage delayed = session.createTextMessage("persisted-transactional-delayed-restart", Message::PERSISTENT);
    producer->send(delayed, SendOptions{Message::PERSISTENT, 4, 0, kDelayMs});
    session.commit();
    commitTime.update();
    // Exchange goes out of scope here -> destructor runs, simulating a restart.
  }

  tiny_mq::Exchange ex2(dir);
  Connection conn2(ex2);
  Session &session2 = conn2.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue2 = session2.createDestination(tiny_mq::destination::Queue, destName);
  Consumer::Ptr consumer2 = session2.createConsumer(queue2);

  const int64_t elapsedMs = commitTime.elapsed() / 1000;
  if (elapsedMs < kDelayMs) {
    EXPECT_EQ(consumer2->recv(50000), nullptr)
        << "delayed persistent transactional message must stay invisible across restart "
           "until its commit-resolved delivery time";
  }

  auto received = Message::As<TextMessage>(consumer2->recv(2000000));
  ASSERT_NE(received, nullptr) << "message must eventually be delivered after restart";
  EXPECT_EQ("persisted-transactional-delayed-restart", received->text());

  ASSERT_NE(received->jmsHeaders.deliveryTime, 0)
      << "JMSDeliveryTime must survive persistence + restart, not come back as 0";
  const int64_t expectedDeliveryTimeMs = commitTime.epochMicroseconds() / 1000 + kDelayMs;
  EXPECT_NEAR(static_cast<double>(received->jmsHeaders.deliveryTime),
              static_cast<double>(expectedDeliveryTimeMs), 5000.0)
      << "JMSDeliveryTime must match the commit-time-resolved value after restart";
}

// Round-2 fix (N1): an absurdly large deliveryDelay/timeToLive must be
// rejected up front rather than silently overflowing nowMs + delay (or the
// millisecond -> nanosecond conversion inside DeliveryScheduler's wait_until)
// into a negative deliveryTime, which would deliver the message immediately —
// the opposite of what was requested.
TEST_F(DeliveryDelayTest, testDeliveryDelayUpperBoundRejected) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);

  TextMessage msg = session.createTextMessage("too-far-out");
  EXPECT_THROW(
      producer->send(msg, SendOptions{Message::NOT_PERSISTENT, 4, 0, std::numeric_limits<int64_t>::max()}),
      std::invalid_argument)
      << "an absurdly large deliveryDelay must be rejected, not silently overflow "
         "into immediate delivery";

  EXPECT_THROW(
      producer->setDefault(SendOptions{Message::NOT_PERSISTENT, 4, std::numeric_limits<int64_t>::max(), 0}),
      std::invalid_argument)
      << "the same upper bound must apply to timeToLive, checked via setDefault";
}

// Round-3 fix (B4): validateSendOptions only guards the SendOptions ingress.
// jmsHeaders.deliveryTime is a public field, and Producer::send(message)
// without setDefault() never calls applyOptions/validateSendOptions at all —
// exactly the path the review's end-to-end reproduction used
// (Exchange -> Session -> producer->send(m) with m.jmsHeaders.deliveryTime
// set directly, no SendOptions in sight). Pre-fix, deliveryTime = INT64_MAX
// reaches DeliveryScheduler::run() unclamped: "now() + milliseconds(delta)"
// overflows system_clock's native duration and wraps into the past,
// wait_until returns instantly, nothing is actually due, and the worker spins
// at 100% CPU while holding _mutex — so stop() (called from ~Destination)
// blocks on that same mutex forever and the destination can never be torn
// down. Run teardown on a separate thread with a bounded join so that if this
// regresses, the test fails fast (and diagnosably) instead of hanging the
// whole suite.
TEST_F(DeliveryDelayTest, testAbsurdDeliveryTimeDoesNotHangTeardown) {
  auto exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir() + "/absurd-delivery-time");
  {
    Connection conn(*exchange);
    Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    Producer::Ptr producer = session.createProducer(queue);

    // Public field, no SendOptions involved — bypasses validateSendOptions
    // entirely, unlike testDeliveryDelayUpperBoundRejected above.
    TextMessage msg = session.createTextMessage("absurd");
    msg.jmsHeaders.deliveryTime = std::numeric_limits<int64_t>::max();
    producer->send(msg);
  }

  std::atomic<bool> destroyed{false};
  std::thread teardown([&exchange, &destroyed] {
    exchange.reset();  // destroys Destination -> DeliveryScheduler::stop()
    destroyed.store(true);
  });

  // Bounded wait: pre-fix, this loop always exhausts its budget because
  // exchange.reset() never returns. Post-fix this takes on the order of
  // milliseconds. 5s total budget is generous margin on any CI machine while
  // still keeping a hung run from stalling the suite indefinitely.
  for (int i = 0; i < 100 && !destroyed.load(); ++i) {
    Poco::Thread::sleep(50);
  }
  EXPECT_TRUE(destroyed.load())
      << "destination teardown must complete in finite time even for an absurd "
         "deliveryTime — pre-fix this hangs forever on stop()'s mutex while the "
         "worker spins at 100% CPU (spec 13 review round 2, B4)";

  if (destroyed.load()) {
    teardown.join();
  } else {
    // Never observed to complete: joining here would hang this test process
    // too. Detach and let the process exit reclaim it; the EXPECT_TRUE above
    // already recorded the failure.
    teardown.detach();
  }
}

// Round-3 fix (N15): DeliveryScheduler::enqueueOrSchedule clamps an absurd
// deliveryTime to 0 and fixes up the header so a consumer doesn't see a bogus
// JMSDeliveryTime on a message actually delivered immediately. For a
// PERSISTENT message, Consumer::preparePush caches this message's serialized
// bytes (via toBytes()) *before* the clamp runs, and Consumer::recv's fast
// path rehydrates jmsHeaders from that cache (fromBytes) on receipt — so
// patching only the live header without also patching the cached bytes
// (Message::patchCachedDeliveryTime) reverts the fix-up the instant the
// consumer receives the message. Same defect class as round 2's B2
// recurring in new code (docs/reviews/13-delivery-delay.review.md, round 3).
TEST_F(DeliveryDelayTest, testAbsurdDeliveryTimePersistentMessageClampedOnReceipt) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  // Public field, no SendOptions involved — bypasses validateSendOptions
  // entirely, same ingress as testAbsurdDeliveryTimeDoesNotHangTeardown, but
  // PERSISTENT this time so preparePush's cached-bytes path is exercised.
  TextMessage msg = session.createTextMessage("absurd-persistent");
  msg.reliability = Message::PERSISTENT;
  msg.jmsHeaders.deliveryTime = std::numeric_limits<int64_t>::max();
  producer->send(msg);

  // Clamped to "immediate": must be visible right away, not scheduled ~forever.
  Message::Ptr received = consumer->recv(5000000);
  ASSERT_NE(received, nullptr) << "an absurd deliveryTime must be clamped to immediate "
                                   "delivery, not silently scheduled far in the future";
  EXPECT_EQ(received->jmsHeaders.deliveryTime, 0)
      << "JMSDeliveryTime seen by the application must be the clamped value (0), not the "
         "original garbage restored from preparePush's pre-clamp cached bytes";
}

// Round-3 fix (B4, continued): direct, deterministic test of the exact
// bounded-delta arithmetic run() uses to pick wait_until's deadline — no
// threading involved, so this pins down the fix's correctness independently
// of the end-to-end test above (which also benefits from
// DeliveryScheduler::enqueueOrSchedule's own ingress clamp and so alone
// wouldn't prove this specific arithmetic is what's protecting run()).
TEST_F(DeliveryDelayTest, testSchedulerWaitDeltaCappedAgainstOverflow) {
  using tiny_mq::DeliveryScheduler;
  const int64_t now = Poco::Timestamp().epochMicroseconds() / 1000;

  // The exact value the review's end-to-end repro used. Uncapped, this would
  // be a delta of roughly INT64_MAX - now milliseconds — enough to overflow
  // system_clock's native (finer-than-millisecond) duration on both libc++
  // (microseconds) and libstdc++ (nanoseconds).
  const int64_t delta = DeliveryScheduler::cappedWaitDeltaMs(std::numeric_limits<int64_t>::max(), now);
  EXPECT_GE(delta, 0);
  EXPECT_LE(delta, DeliveryScheduler::kMaxWaitMs)
      << "an absurd deadline must never produce an uncapped wait delta";

  // The capped delta must actually be safe to feed into the same conversion
  // run() performs: the resulting deadline must not be in the past (which is
  // exactly the failure mode that caused wait_until to return instantly and
  // the worker to spin).
  const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(delta);
  EXPECT_GE(deadline, std::chrono::system_clock::now())
      << "a capped delta must never yield a deadline in the past";

  // A deadline already in the past must clamp to 0, not go negative.
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(now - 10000, now), 0);

  // An ordinary near-future deadline must be passed through unchanged — the
  // cap must not needlessly coarsen legitimate short delays.
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(now + 500, now), 500);

  // Exact boundary: deadline exactly kMaxWaitMs away must return the cap
  // unchanged (not one-off), and one millisecond inside the boundary must
  // return the uncapped, exact delta — pins the fixed comparison at the
  // boundary rather than just checking "small" and "huge" inputs.
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(now + DeliveryScheduler::kMaxWaitMs, now),
            DeliveryScheduler::kMaxWaitMs);
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(now + DeliveryScheduler::kMaxWaitMs - 1, now),
            DeliveryScheduler::kMaxWaitMs - 1);

  // Values right at the signed 64-bit edge: the old implementation computed
  // deadlineMs - nowMsValue first, which is signed overflow (UB) for any of
  // these and is exactly what let the optimizer delete the "delta < 0"
  // guard. The fixed comparison must never form that overflowing
  // subtraction and must still clamp correctly.
  const int64_t maxV = std::numeric_limits<int64_t>::max();
  const int64_t minV = std::numeric_limits<int64_t>::min();
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(maxV, now), DeliveryScheduler::kMaxWaitMs);
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(maxV - 1, now), DeliveryScheduler::kMaxWaitMs);
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(minV, now), 0);
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(minV + 1, now), 0);

  // A "now" reading near the top of the range (as if the system clock were
  // implausibly far in the future) combined with a huge deadline must still
  // clamp without overflowing the nowMsValue + kMaxWaitMs guard itself.
  const int64_t hugeNow = maxV - DeliveryScheduler::kMaxWaitMs - 1;
  EXPECT_EQ(DeliveryScheduler::cappedWaitDeltaMs(maxV, hugeNow), DeliveryScheduler::kMaxWaitMs);
}
