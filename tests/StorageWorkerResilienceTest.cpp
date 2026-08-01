//
// LP-03 — ConcurrentLinearStorage worker thread must never crash the process.
//

#include "ConcurrentLinearStorage.h"
#include "Connection.h"
#include "Exchange.h"
#include "Producer.h"
#include "Session.h"
#include "TextMessage.h"
#include <gtest/gtest.h>
#include <Poco/Channel.h>
#include <Poco/File.h>
#include <Poco/Format.h>
#include <Poco/Logger.h>
#include <Poco/Message.h>
#include <Poco/Thread.h>
#include <Poco/UUIDGenerator.h>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

using tiny_mq::Connection;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::TextMessage;

namespace {
// Captures log messages so a test can assert an error was recorded rather than
// silently swallowed.
class CapturingChannel : public Poco::Channel {
 public:
  void log(const Poco::Message& msg) override {
    std::lock_guard<std::mutex> lock(_mutex);
    _messages.push_back(msg.getText());
  }
  std::vector<std::string> messages() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _messages;
  }

 private:
  mutable std::mutex _mutex;
  std::vector<std::string> _messages;
};
}  // namespace

// Test plan #1: provoke a storage-operation failure in a running worker and
// confirm the process survives — the error is logged with operation context
// instead of escaping run() and crashing via std::terminate.
//
// Storage::tom(id) does `_tom.at(id)`, which throws std::out_of_range for a
// tomId that was never created. Before the LP-03 fix, run() had no try/catch
// around operation dispatch, so this exception would propagate out of the
// worker thread and abort the whole process.
TEST(StorageWorkerResilienceTest, InvalidOperationIsLoggedNotFatal) {
  const std::string basePath = "./tiny-mq-lp03-resilience";
  {
    Poco::File pre(basePath);
    if (pre.exists()) pre.remove(true);
  }

  auto storageId = Poco::UUIDGenerator::defaultGenerator().createRandom();
  linear_storage::ConcurrentLinearStorage storage(storageId, basePath);

  auto& logger = Poco::Logger::get(Poco::format("tiny_mq.cuncurrent_storge.%s", storageId));
  auto* channel = new CapturingChannel;
  logger.setChannel(channel);
  logger.setLevel(Poco::Message::PRIO_TRACE);

  storage.start();

  // Bogus tomId — never created in this storage — forces Storage::tom() to throw.
  auto record = storage.record(/*tomId=*/999999, /*offset=*/0);
  EXPECT_EQ(record.tomId, std::numeric_limits<Poco::UInt32>::max())
      << "the failed operation must resolve to the default (not-found) Record, "
         "not hang the caller forever";

  bool foundLoggedError = false;
  for (const auto& text : channel->messages()) {
    if (text.find("GET_RECORD_BY_TOM_OFFSET") != std::string::npos) {
      foundLoggedError = true;
      break;
    }
  }
  EXPECT_TRUE(foundLoggedError) << "the operation failure must be logged with its operation context";

  // The worker thread must still be alive and able to serve further requests —
  // proof the exception did not silently kill the loop mid-iteration either.
  auto uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  std::vector<char> data{'x'};
  auto appended = storage.append(uuid, data);
  EXPECT_NE(appended.tomId, std::numeric_limits<Poco::UInt32>::max())
      << "the worker must keep serving operations after logging a failure";

  storage.stop();
  Poco::File dir(basePath);
  if (dir.exists()) dir.remove(true);
}

// Test plan #2: after Exchange is destroyed, no storage worker thread is still
// alive and touching the filesystem — the on-disk directory can be removed
// without any exception surfacing on the worker thread (which would otherwise
// crash the process per the original LP-03 symptom).
TEST(StorageWorkerResilienceTest, NoWorkerThreadSurvivesExchangeDestruction) {
  const std::string basePath = "./tiny-mq-lp03-shutdown-order";
  {
    Poco::File pre(basePath);
    if (pre.exists()) pre.remove(true);
  }

  auto exchange = std::make_unique<tiny_mq::Exchange>(basePath);
  {
    Connection conn(*exchange);
    Session& session = conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, "lp03shutdown");
    Producer::Ptr producer = session.createProducer(destination);
    Consumer::Ptr consumer = session.createConsumer(destination);

    TextMessage message = session.createTextMessage("probe", Message::PERSISTENT);
    producer->send(message);
    auto received = Message::As<TextMessage>(consumer->recv());
    ASSERT_NE(received, nullptr);
    consumer->acknowledgeOn(*received);  // triggers removeAsync (fire-and-forget)
  }
  exchange.reset();

  // If any worker thread survived Exchange destruction, it would still be
  // sweeping on its default 1 s deadline and would throw when it hits this
  // now-missing directory.
  Poco::File dir(basePath);
  ASSERT_NO_THROW(dir.remove(true)) << "directory removal must not race a live storage worker";

  Poco::Thread::sleep(1500);  // past the default sweep interval — surface any surviving thread
  SUCCEED() << "process survived — no leaked worker thread touched the removed directory";
}
