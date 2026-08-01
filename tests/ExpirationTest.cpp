//
// JMSExpiration tests for tiny-mq (JMS spec 44 — message expiration sweep).
//

#include "ExpirationTest.h"
#include "ConcurrentLinearStorage.h"
#include "Connection.h"
#include "Exchange.h"
#include "Producer.h"
#include "Session.h"
#include "TextMessage.h"
#include "TestHelper.h"
#include <Poco/File.h>
#include <Poco/Thread.h>
#include <Poco/Timestamp.h>
#include <Poco/UUIDGenerator.h>
#include <utility>
#include <vector>

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::SendOptions;
using tiny_mq::Session;
using tiny_mq::TextMessage;

void ExpirationTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void ExpirationTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

static int64_t nowMs() { return Poco::Timestamp().epochMicroseconds() / 1000; }

// Test plan #1: an expired message is never delivered — recv() drops it and
// returns the next live message instead.
TEST_F(ExpirationTest, testExpiredMessageDroppedOnRecv) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  // TTL = 100 ms; this one must lapse before we recv.
  TextMessage expiring = session.createTextMessage("expired");
  producer->send(expiring, SendOptions{Message::NOT_PERSISTENT, 4, 100, 0});
  // timeToLive = 0 → never expires.
  TextMessage live = session.createTextMessage("live");
  producer->send(live, SendOptions{Message::NOT_PERSISTENT, 4, 0, 0});

  Poco::Thread::sleep(200);  // let the first message's TTL lapse

  auto received = Message::As<TextMessage>(consumer->recv(200000));
  ASSERT_NE(received, nullptr) << "the live message must still be delivered";
  EXPECT_EQ("live", received->text()) << "the expired message must be dropped, not delivered";

  auto none = consumer->recv(50000);
  EXPECT_EQ(none, nullptr) << "no further messages expected after the live one";
}

// Test plan #2: expired persistent records are removed from ConcurrentLinearStorage
// by the background sweeper within one sweep interval; live records are kept.
TEST_F(ExpirationTest, testExpiredMessageSweptFromStorage) {
  const std::string basePath = "./tiny-mq-test-storage/ExpirationTest-sweep";
  {
    auto storageId = Poco::UUIDGenerator::defaultGenerator().createRandom();
    linear_storage::ConcurrentLinearStorage storage(storageId, basePath);
    storage.setSweepIntervalMicros(50'000);  // 50 ms so the test stays fast
    storage.start();

    // Build a stored record exactly as preparePush() does: [1-byte type][0x02 payload].
    auto makeStored = [](const std::string &body, int64_t expiration) {
      auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
      TextMessage msg(uuid, body, Message::PERSISTENT);
      msg.jmsHeaders.expiration = expiration;
      auto payload = msg.toBytes();
      std::vector<char> data;
      data.reserve(1 + payload.size());
      data.push_back(static_cast<char>(msg.type()));
      data.insert(data.end(), payload.begin(), payload.end());
      return std::make_pair(uuid, data);
    };

    auto expired = makeStored("expired", nowMs() - 1000);  // already in the past
    auto live = makeStored("live", 0);                     // never expires

    storage.append(expired.first, expired.second);
    storage.append(live.first, live.second);
    ASSERT_EQ(storage.scan().size(), 2u) << "both records must be present before the sweep";

    Poco::Thread::sleep(250);  // several sweep intervals

    EXPECT_EQ(storage.scan().size(), 1u)
        << "the expired record must be swept from storage, the live one kept";

    storage.stop();
  }
  Poco::File dir(basePath);
  if (dir.exists()) dir.remove(true);
}

// m2: the persistent drop path on recv (resolve the storage record, removeAsync)
// — the most intricate branch of the change, not exercised by the NOT_PERSISTENT
// tests above.
TEST_F(ExpirationTest, testExpiredPersistentMessageDroppedOnRecv) {
  Connection conn(*_exchange);
  Session &session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  Destination::Ptr queue = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  Producer::Ptr producer = session.createProducer(queue);
  Consumer::Ptr consumer = session.createConsumer(queue);

  TextMessage expiring = session.createTextMessage("expired");
  producer->send(expiring, SendOptions{Message::PERSISTENT, 4, 100, 0});
  TextMessage live = session.createTextMessage("live");
  producer->send(live, SendOptions{Message::PERSISTENT, 4, 0, 0});

  Poco::Thread::sleep(200);

  auto received = Message::As<TextMessage>(consumer->recv(200000));
  ASSERT_NE(received, nullptr) << "the live persistent message must still be delivered";
  EXPECT_EQ("live", received->text()) << "the expired persistent message must be dropped";
  EXPECT_EQ(consumer->recv(50000), nullptr) << "no further messages expected";
}

// B1: the sweep must fire under a *continuous* operation stream, not only when the
// worker queue goes fully idle. With the idle-only cadence this record was never
// reclaimed; with the deadline-based cadence it is.
TEST_F(ExpirationTest, testSweepFiresUnderContinuousLoad) {
  const std::string basePath = "./tiny-mq-test-storage/ExpirationTest-load";
  {
    auto storageId = Poco::UUIDGenerator::defaultGenerator().createRandom();
    linear_storage::ConcurrentLinearStorage storage(storageId, basePath);
    storage.setSweepIntervalMicros(50'000);  // 50 ms
    storage.start();

    auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
    TextMessage msg(uuid, "expired", Message::PERSISTENT);
    msg.jmsHeaders.expiration = nowMs() - 1000;  // already in the past
    auto payload = msg.toBytes();
    std::vector<char> data;
    data.reserve(1 + payload.size());
    data.push_back(static_cast<char>(msg.type()));
    data.insert(data.end(), payload.begin(), payload.end());
    storage.append(uuid, data);
    ASSERT_EQ(storage.scan().size(), 1u);

    // Drive a steady stream of synchronous ops so the worker never sees a full
    // idle interval — the sweep must still fire on its deadline.
    Poco::Timestamp start;
    while (start.elapsed() < 400'000) {  // 400 ms of continuous traffic
      (void)storage.record(uuid);
    }

    EXPECT_EQ(storage.scan().size(), 0u)
        << "sweep must reclaim the expired record even under continuous load";

    storage.stop();
  }
  Poco::File dir(basePath);
  if (dir.exists()) dir.remove(true);
}
