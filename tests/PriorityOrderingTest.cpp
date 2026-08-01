// JMSPriority ordering tests for tiny-mq (spec 45 — priority ordering on dequeue).
//
// Test plan:
//   (a) testInterleavedPriority: interleaved send of p=0 and p=9; receiver
//       sees ALL p=9 messages before any p=0 message.
//   (b) testUniformPriorityIsFIFO: uniform-priority workload (p=4) remains
//       FIFO — order of receipt matches order of send.
//   (c) testRestartPreservesBanding: persistent messages survive an Exchange
//       restart and are restored into the correct priority band.

#include "PriorityOrderingTest.h"
#include "Connection.h"
#include "Exchange.h"
#include "Producer.h"
#include "Session.h"
#include "TextMessage.h"
#include "TestHelper.h"
#include <Poco/File.h>
#include <vector>
#include <string>

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::SendOptions;
using tiny_mq::Session;
using tiny_mq::TextMessage;

void PriorityOrderingTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void PriorityOrderingTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

// ─── (a) Interleaved priority ──────────────────────────────────────────────
//
// Send N messages alternating between p=0 and p=9.  After all are queued,
// drain the consumer and verify: the first N/2 received are ALL p=9, the
// second N/2 are ALL p=0.  Within each band the original send order is kept.
TEST_F(PriorityOrderingTest, testInterleavedPriority) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int kN = 10;  // must be even
  // Send kN messages alternating p=9 / p=0
  for (int i = 0; i < kN; ++i) {
    int32_t prio = (i % 2 == 0) ? 9 : 0;
    std::string body = "msg-" + std::to_string(i);
    TextMessage msg = session.createTextMessage(body, Message::NOT_PERSISTENT);
    producer->send(msg, SendOptions{Message::NOT_PERSISTENT, prio, 0, 0});
  }

  // Receive all kN messages and split by priority
  std::vector<int32_t> order;
  order.reserve(kN);
  for (int i = 0; i < kN; ++i) {
    auto recv = consumer->recv(200000);
    ASSERT_NE(recv, nullptr) << "expected message " << i;
    order.push_back(recv->jmsHeaders.priority);
  }

  // First half must all be p=9
  for (int i = 0; i < kN / 2; ++i) {
    EXPECT_EQ(9, order[static_cast<size_t>(i)])
        << "position " << i << " must be p=9 (got " << order[static_cast<size_t>(i)] << ")";
  }
  // Second half must all be p=0
  for (int i = kN / 2; i < kN; ++i) {
    EXPECT_EQ(0, order[static_cast<size_t>(i)])
        << "position " << i << " must be p=0 (got " << order[static_cast<size_t>(i)] << ")";
  }
}

// ─── (b) Uniform priority remains FIFO ────────────────────────────────────
//
// Send M messages all at the same priority (default p=4).  Received order
// must exactly match send order — priority bands must not perturb FIFO.
TEST_F(PriorityOrderingTest, testUniformPriorityIsFIFO) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int kM = 20;
  std::vector<std::string> bodies;
  bodies.reserve(kM);
  for (int i = 0; i < kM; ++i) {
    bodies.push_back("fifo-" + std::to_string(i));
    TextMessage msg = session.createTextMessage(bodies.back(), Message::NOT_PERSISTENT);
    producer->send(msg, SendOptions{Message::NOT_PERSISTENT, 4, 0, 0});
  }

  for (int i = 0; i < kM; ++i) {
    auto recv = Message::As<TextMessage>(consumer->recv(200000));
    ASSERT_NE(recv, nullptr) << "expected message " << i;
    EXPECT_EQ(bodies[static_cast<size_t>(i)], recv->text())
        << "message " << i << " out of FIFO order";
  }
}

// ─── (b') Adjacent-priority interleave preserves per-band FIFO ───────────
//
// Send 5×p=4 and 5×p=5 interleaved (adjacent priorities, not the extremes
// 0/9, to catch off-by-one errors in band indexing).  Verify: (1) all p=5
// messages arrive before any p=4 message (inter-band ordering), and (2)
// within each band, receipt order matches send order (intra-band FIFO —
// this is the actual "uniform priority remains FIFO" criterion; a plain
// FIFO queue with no priority awareness would also pass test (b) above, but
// only a queue that keeps each band internally FIFO passes this one).
TEST_F(PriorityOrderingTest, testAdjacentPriorityInterleavePreservesBandFIFO) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  constexpr int kPerBand = 5;
  std::vector<std::string> p4Bodies;
  std::vector<std::string> p5Bodies;
  p4Bodies.reserve(kPerBand);
  p5Bodies.reserve(kPerBand);

  // Interleave sends: p=4, p=5, p=4, p=5, ...
  for (int i = 0; i < kPerBand; ++i) {
    std::string body4 = "p4-" + std::to_string(i);
    TextMessage msg4 = session.createTextMessage(body4, Message::NOT_PERSISTENT);
    producer->send(msg4, SendOptions{Message::NOT_PERSISTENT, 4, 0, 0});
    p4Bodies.push_back(std::move(body4));

    std::string body5 = "p5-" + std::to_string(i);
    TextMessage msg5 = session.createTextMessage(body5, Message::NOT_PERSISTENT);
    producer->send(msg5, SendOptions{Message::NOT_PERSISTENT, 5, 0, 0});
    p5Bodies.push_back(std::move(body5));
  }

  std::vector<std::string> received;
  received.reserve(2 * kPerBand);
  for (int i = 0; i < 2 * kPerBand; ++i) {
    auto recv = Message::As<TextMessage>(consumer->recv(200000));
    ASSERT_NE(recv, nullptr) << "expected message " << i;
    received.push_back(recv->text());
  }

  // Inter-band: all p=5 (higher priority) before any p=4.
  for (int i = 0; i < kPerBand; ++i) {
    EXPECT_EQ(p5Bodies[static_cast<size_t>(i)], received[static_cast<size_t>(i)])
        << "position " << i << " must be the next p=5 message in send order";
  }
  // Intra-band: p=4 messages follow, in original send order.
  for (int i = 0; i < kPerBand; ++i) {
    EXPECT_EQ(p4Bodies[static_cast<size_t>(i)], received[static_cast<size_t>(kPerBand + i)])
        << "position " << (kPerBand + i) << " must be the next p=4 message in send order";
  }
}

// ─── (c) Restart preserves priority banding ───────────────────────────────
//
// Send persistent messages with mixed priorities (p=0, p=9) then destroy the
// Exchange (simulating a restart).  Create a new Exchange on the same path,
// create a consumer, and verify that all p=9 messages arrive before p=0.
TEST_F(PriorityOrderingTest, testRestartPreservesBanding) {
  const std::string destName = std::string(CurrentTestName) + "_queue";
  const std::string dir = CurrentTestSuiteStorageDir() + "/restart-priority-test";

  constexpr int kLow  = 5;
  constexpr int kHigh = 5;

  // Phase 1: send persistent messages and then let the Exchange die.
  {
    tiny_mq::Exchange ex(dir);
    Connection conn(ex);
    Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, destName);
    Producer::Ptr producer = session.createProducer(queue);

    // Interleave p=0 and p=9 sends.
    for (int i = 0; i < kLow + kHigh; ++i) {
      int32_t prio = (i % 2 == 0) ? 9 : 0;
      TextMessage msg = session.createTextMessage("restart-" + std::to_string(i), Message::PERSISTENT);
      producer->send(msg, SendOptions{Message::PERSISTENT, prio, 0, 0});
    }
    // Exchange goes out of scope → destructor runs, storage is flushed.
  }

  // Phase 2: restart — new Exchange on the same path.
  {
    tiny_mq::Exchange ex(dir);
    Connection conn(ex);
    Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, destName);
    Producer::Ptr producer = session.createProducer(queue);
    Consumer::Ptr consumer = session.createConsumer(queue);

    // Drain all messages and record their priorities.
    std::vector<int32_t> order;
    for (int i = 0; i < kLow + kHigh; ++i) {
      auto recv = consumer->recv(500000);
      ASSERT_NE(recv, nullptr) << "expected message " << i << " after restart";
      order.push_back(recv->jmsHeaders.priority);
      consumer->acknowledgeOn(*recv);
    }

    // All p=9 must precede all p=0 after restart.
    for (int i = 0; i < kHigh; ++i) {
      EXPECT_EQ(9, order[static_cast<size_t>(i)])
          << "post-restart position " << i << " must be p=9";
    }
    for (int i = kHigh; i < kLow + kHigh; ++i) {
      EXPECT_EQ(0, order[static_cast<size_t>(i)])
          << "post-restart position " << i << " must be p=0";
    }
  }
}

// ─── Transactional delivery also respects priority bands ──────────────────
//
// Verify that deliverCommitted (SESSION_TRANSACTED commit path) also routes
// messages into the correct priority band.  This exercises the second durable
// path (Destination::deliverCommitted).
TEST_F(PriorityOrderingTest, testTransactionalPriorityOrdering) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  // Send interleaved p=0 / p=9 messages within a transaction.
  constexpr int kN = 6;
  for (int i = 0; i < kN; ++i) {
    int32_t prio = (i % 2 == 0) ? 9 : 0;
    TextMessage msg = session.createTextMessage("tx-" + std::to_string(i), Message::NOT_PERSISTENT);
    producer->send(msg, SendOptions{Message::NOT_PERSISTENT, prio, 0, 0});
  }
  session.commit();  // triggers deliverCommitted for all staged messages

  // Receive and verify: all p=9 must come first.
  std::vector<int32_t> order;
  for (int i = 0; i < kN; ++i) {
    auto recv = consumer->recv(200000);
    ASSERT_NE(recv, nullptr) << "expected message " << i;
    order.push_back(recv->jmsHeaders.priority);
  }
  session.commit();  // ack the received messages

  for (int i = 0; i < kN / 2; ++i) {
    EXPECT_EQ(9, order[static_cast<size_t>(i)])
        << "transactional position " << i << " must be p=9";
  }
  for (int i = kN / 2; i < kN; ++i) {
    EXPECT_EQ(0, order[static_cast<size_t>(i)])
        << "transactional position " << i << " must be p=0";
  }
}
