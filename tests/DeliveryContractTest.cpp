#include "Connection.h"
#include "DeliveryClock.h"
#include "DestinationHash.h"
#include "Exceptions.h"
#include "TestHelper.h"
#include <Poco/Environment.h>
#include <Poco/Process.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <latch>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace tiny_mq {
namespace internal {
void observeCommitPreparation(const std::function<void()>* observer);
}
namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;
constexpr int64_t startTime = 20'000;
constexpr int crashCode = 72;

void require(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string body(const Message::Ptr& message) {
  if (auto text = Message::As<TextMessage>(message)) {
    return text->text();
  }
  return {};
}

void send(Session& session, Producer& producer, const std::string& text,
          Message::Reliability reliability = Message::PERSISTENT) {
  producer.send(session.createTextMessage(text, reliability));
}

class DeliveryContractTest : public testing::Test {
 protected:
  const std::string directory = CurrentTestSuiteStorageDir() + "/" + CurrentTestName + "/";
  void SetUp() override {
    RemoveTestStorageDir(directory);
    DeliveryClock::setTestTime(startTime);
  }
  void TearDown() override {
    DeliveryClock::setTestTime(-1);
    RemoveTestStorageDir(directory);
  }
};

class Injection {
 public:
  explicit Injection(const char* variable)
      : _variable(variable), _previous(Poco::Environment::get(variable, "")) {
    Poco::Environment::set(_variable, "1");
  }
  ~Injection() { Poco::Environment::set(_variable, _previous); }
 private:
  const char* _variable;
  std::string _previous;
};

std::string executable;
int child(const std::string& directory, const std::string& scenario, const std::string& point = {}) {
  const auto status = Poco::Process::launch(executable, {"--delivery-test-child", directory, scenario},
                     nullptr, nullptr, nullptr, {{"TINY_MQ_TEST_CRASH", point}}).wait();
  std::cout << "  child " << scenario << " " << point << " exit=" << status << std::endl;
  return status;
}

#ifndef _WIN32
std::string readScratch(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.is_open(), "cannot open scratch file");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

fs::path retryAttempts(const fs::path& root) {
  return root / std::to_string(destination::hash(destination::Queue, "origin")) / "attempt-72";
}
#endif

void legacyRecord(const std::string& directory, const std::string& name, int64_t expiration = 0,
                  bool extended = false) {
  const auto path = Poco::Path(directory).append(std::to_string(destination::hash(destination::Queue, name))).makeDirectory();
  const Poco::UUID storageNamespace("a3b4c5d6-e7f8-4a5b-8c9d-0e1f2a3b4c5d");
  auto& ids = Poco::UUIDGenerator::defaultGenerator();
  {
    linear_storage::Storage storage(ids.createFromName(storageNamespace, "queue://" + name), path);
    TextMessage message(ids.createRandom(), "legacy", Message::PERSISTENT);
    message.jmsHeaders.timestamp = 172;
    message.jmsHeaders.expiration = expiration;
    if (extended) {
      message.jmsHeaders.setCorrelationId(BytesVector{0, -1});
    }
    auto bytes = message.toBytes();
    EXPECT_EQ(extended ? 0x03 : 0x02, bytes.front());
    bytes.insert(bytes.begin(), Message::TEXT_MESSAGE);
    storage.append(message.uuid, {bytes.begin(), bytes.end()});
  }
  const fs::path volume(path.toString());
  fs::remove(volume / "1.delivery-72");
  std::fstream oldPadding(volume / "1.tom", std::ios::binary | std::ios::in | std::ios::out);
  oldPadding.seekp(offsetof(linear_storage::Header, attempt));
  oldPadding.put(char{42});

}

// REQ-72-1
TEST_F(DeliveryContractTest, SendClockMatrix) {
  for (auto reliability : {Message::PERSISTENT, Message::NOT_PERSISTENT}) {
    for (bool transacted : {false, true}) {
      SCOPED_TRACE(testing::Message() << reliability << "/" << transacted);
      Exchange exchange(directory);
      Connection connection(exchange);
      connection.start();
      auto& session = connection.createSession(transacted ? Session::SESSION_TRANSACTED : Session::AUTO_ACKNOWLEDGE);
      auto& observer = connection.createSession();
      const auto queue = session.createDestination(destination::Queue,
          "clock-" + std::to_string(reliability) + "-" + std::to_string(transacted));
      auto producer = session.createProducer(queue);
      auto consumer = observer.createConsumer(queue);
      for (const auto offset : {5, 15}) {
        DeliveryClock::setTestTime(startTime);
        auto message = session.createTextMessage("delayed");
        producer->send(message, {reliability, 8, 20, 10});
        EXPECT_EQ(startTime, message.jmsHeaders.timestamp);
        EXPECT_EQ(startTime + 10, message.jmsHeaders.deliveryTime);
        EXPECT_EQ(startTime + 20, message.jmsHeaders.expiration);
        DeliveryClock::setTestTime(startTime + offset);
        if (transacted) {
          EXPECT_FALSE(consumer->receiveNoWait());
          session.commit();
        }
        if (offset < 10) {
          EXPECT_FALSE(consumer->receiveNoWait());
        }
        DeliveryClock::setTestTime(startTime + std::max(offset, 10));
        auto incoming = consumer->receive(1000);
        ASSERT_TRUE(incoming);
        EXPECT_EQ(message.jmsHeaders.timestamp, incoming->jmsHeaders.timestamp);
        EXPECT_EQ(message.jmsHeaders.deliveryTime, incoming->jmsHeaders.deliveryTime);
        EXPECT_EQ(message.jmsHeaders.expiration, incoming->jmsHeaders.expiration);
        EXPECT_EQ(8, incoming->jmsHeaders.priority);
      }
      auto expired = session.createTextMessage("expired");
      producer->send(expired, {reliability, 4, 10, 10});
      if (transacted) {
        session.commit();
      }
      DeliveryClock::setTestTime(expired.jmsHeaders.expiration);
      EXPECT_FALSE(consumer->receive(15));
      auto zero = session.createTextMessage("zero");
      producer->send(zero, {reliability, 4, 0, 0});
      if (transacted) {
        session.commit();
      }
      auto incoming = consumer->receiveNoWait();
      ASSERT_TRUE(incoming);
      EXPECT_EQ(zero.jmsHeaders.timestamp, incoming->jmsHeaders.deliveryTime);
      EXPECT_EQ(0, incoming->jmsHeaders.expiration);

      producer->send(zero, {reliability, 4, INT64_MAX, INT64_MAX});
      if (transacted) {
        session.commit();
      }
      EXPECT_EQ(INT64_MAX, zero.jmsHeaders.expiration);
      EXPECT_EQ(INT64_MAX, zero.jmsHeaders.deliveryTime);
      EXPECT_FALSE(consumer->receiveNoWait());
    }
  }
  DeliveryClock::setTestTime(startTime);
  legacyRecord(directory, "legacy");
  legacyRecord(directory, "legacy-extended", 0, true);
  {
    Exchange exchange(directory);
    Connection connection(exchange);
    auto& session = connection.createSession();
    auto queue = session.createDestination(destination::Queue, "legacy");
    auto writer = session.createProducer(queue);
    auto message = session.createTextMessage("new");
    writer->send(message, {Message::PERSISTENT, 4, 20, 10});
  }
  {
    Exchange exchange(directory);
    Connection connection(exchange);
    connection.start();
    auto& session = connection.createSession();
    auto consumer = session.createConsumer(session.createDestination(destination::Queue, "legacy"));
    auto old = consumer->receiveNoWait();
    ASSERT_TRUE(old);
    EXPECT_EQ("legacy", body(old));
    EXPECT_EQ(0, old->jmsHeaders.deliveryTime);
    EXPECT_EQ(172, old->jmsHeaders.timestamp);
    EXPECT_EQ(1, old->jmsHeaders.jmsDeliveryCount());
    auto queue = session.createDestination(destination::Queue, "legacy-extended");
    auto extended = session.createConsumer(queue)->receiveNoWait();
    ASSERT_TRUE(extended);
    EXPECT_EQ(1, extended->jmsHeaders.jmsDeliveryCount());
    EXPECT_EQ((BytesVector{0, -1}), std::get<BytesVector>(extended->jmsHeaders.getCorrelationId()));
    EXPECT_FALSE(consumer->receiveNoWait());
    DeliveryClock::setTestTime(startTime + 10);
    auto current = consumer->receive(1000);
    ASSERT_TRUE(current);
    EXPECT_EQ("new", body(current));
    EXPECT_EQ(startTime, current->jmsHeaders.timestamp);
    EXPECT_EQ(startTime + 10, current->jmsHeaders.deliveryTime);
    EXPECT_EQ(startTime + 20, current->jmsHeaders.expiration);
  }
}

// REQ-72-2
TEST_F(DeliveryContractTest, ReceiveLifecycle) {
  Exchange exchange(directory);
  Connection connection(exchange);
  auto& session = connection.createSession();
  auto queue = session.createDestination(destination::Queue, "receive");
  auto producer = session.createProducer(queue);
  auto consumer = session.createConsumer(queue);
  EXPECT_FALSE(connection.isStarted());
  EXPECT_THROW(consumer->receive(-1), std::invalid_argument);
  send(session, *producer, "stopped");
  EXPECT_FALSE(consumer->receiveNoWait());
  EXPECT_FALSE(consumer->recv(0));
  const auto began = std::chrono::steady_clock::now();
  EXPECT_FALSE(consumer->receive(15));
  EXPECT_GE(std::chrono::steady_clock::now() - began, 12ms);

  auto blocking = std::async(std::launch::async, [&] { return consumer->receive(0); });
  EXPECT_EQ(std::future_status::timeout, blocking.wait_for(15ms));
  connection.start();
  connection.start();
  EXPECT_EQ("stopped", body(blocking.get()));
  connection.stop();
  connection.stop();
  send(session, *producer, "second", Message::NOT_PERSISTENT);
  EXPECT_FALSE(consumer->recv(2000));
  connection.start();
  EXPECT_EQ("second", body(consumer->recv(-1)));
  EXPECT_FALSE(consumer->receiveNoWait());

  auto waiting = std::async(std::launch::async, [&] { return consumer->receive(); });
  EXPECT_EQ(std::future_status::timeout, waiting.wait_for(15ms));
  consumer->close();
  EXPECT_FALSE(waiting.get());
  EXPECT_THROW(consumer->receiveNoWait(), IllegalStateException);

  // A ready AutoAck receive racing close either claims the message or leaves
  // it for a different consumer. No receive may enter after close returns.
  auto ready = session.createConsumer(queue);
  send(session, *producer, "ready", Message::NOT_PERSISTENT);
  std::latch startRace(2);
  auto readyResult = std::async(std::launch::async, [&] {
    startRace.arrive_and_wait();
    try {
      return ready->receiveNoWait();
    } catch (const IllegalStateException&) {
      return Message::Ptr{};
    }
  });
  startRace.arrive_and_wait();
  ready->close();
  EXPECT_THROW(ready->receiveNoWait(), IllegalStateException);
  auto received = readyResult.get();
  auto remaining = session.createConsumer(queue)->receiveNoWait();
  ASSERT_NE(static_cast<bool>(received), static_cast<bool>(remaining));
  EXPECT_EQ("ready", body(received ? received : remaining));

  // AutoAck checks the body before taking the ready entry. Reentrant receive
  // must reject admission before waiting on the predicate's own queue lock.
  auto automatic = session.createConsumer(queue);
  send(session, *producer, "auto checked", Message::NOT_PERSISTENT);
  const auto autoResult = automatic->receiveChecked([&](const Message&) {
    EXPECT_THROW(automatic->receiveNoWait(), IllegalStateException);
    ready->close();
    return true;
  });
  EXPECT_EQ("auto checked", body(autoResult.message));

  // A checked client receive runs the predicate outside the ready-set lock.
  // Close must still wait for receive to release the session and its claim.
  auto& client = connection.createSession(Session::CLIENT_ACKNOWLEDGE);
  auto checked = client.createConsumer(queue);
  send(session, *producer, "checked", Message::NOT_PERSISTENT);
  std::promise<void> evaluating;
  auto entered = evaluating.get_future();
  std::latch finishCheck(1);
  auto result = std::async(std::launch::async, [&] {
    return checked->receiveChecked([&](const Message&) {
      evaluating.set_value();
      finishCheck.wait();
      return true;
    });
  });
  EXPECT_EQ(std::future_status::ready, entered.wait_for(2s));
  EXPECT_THROW(checked->receiveNoWait(), IllegalStateException);
  std::latch closing(2);
  const auto close = [&] {
    closing.count_down();
    checked->close();
  };
  auto closeFirst = std::async(std::launch::async, close);
  auto closeSecond = std::async(std::launch::async, close);
  closing.wait();
  EXPECT_EQ(std::future_status::timeout, closeFirst.wait_for(15ms));
  EXPECT_EQ(std::future_status::timeout, closeSecond.wait_for(15ms));
  finishCheck.count_down();
  EXPECT_EQ("checked", body(result.get().message));
  closeFirst.get();
  closeSecond.get();
  EXPECT_THROW(checked->receiveNoWait(), IllegalStateException);
  client.acknowledge();

  auto other = session.createConsumer(queue);
  connection.stop();
  auto stopped = std::async(std::launch::async, [&] { return other->recv(-1); });
  EXPECT_EQ(std::future_status::timeout, stopped.wait_for(15ms));
  connection.close();
  EXPECT_FALSE(stopped.get());
  EXPECT_THROW(session.createProducer(queue), IllegalStateException);
}

// REQ-72-3
TEST_F(DeliveryContractTest, ListenerModeMatrix) {
  for (auto mode : {Session::AUTO_ACKNOWLEDGE, Session::CLIENT_ACKNOWLEDGE,
                   Session::DUPS_OK_ACKNOWLEDGE, Session::SESSION_TRANSACTED}) {
    SCOPED_TRACE(mode);
    Exchange exchange(directory);
    Connection connection(exchange);
    auto& session = connection.createSession(mode);
    Connection sending(exchange);
    auto& sender = sending.createSession();
    auto queue = session.createDestination(destination::Queue, "listener-" + std::to_string(mode));
    auto side = session.createDestination(destination::Queue, "side-" + std::to_string(mode));
    auto producer = sender.createProducer(queue);
    auto companion = sender.createProducer(side);
    auto consumer = session.createConsumer(queue);
    auto other = session.createConsumer(side);
    std::promise<void> began, second, sideDelivery;
    std::latch release(1);
    std::atomic<int> active = 0;
    std::atomic<int> calls = 0;
    const bool immediateRetry = mode == Session::AUTO_ACKNOWLEDGE || mode == Session::DUPS_OK_ACKNOWLEDGE;
    consumer->setMessageListener([&](Message::Ptr message) {
      const auto concurrent = active.fetch_add(1);
      EXPECT_EQ(0, concurrent);
      const auto call = ++calls;
      if (call == 1) {
        EXPECT_EQ(1, message->jmsHeaders.jmsDeliveryCount());
        EXPECT_THROW(connection.stop(), IllegalStateException);
        EXPECT_THROW(connection.close(), IllegalStateException);
        EXPECT_THROW(session.close(), IllegalStateException);
        began.set_value();
        release.wait();
        --active;
        throw std::runtime_error("listener failure");
      }
      EXPECT_EQ(immediateRetry ? "first" : "second", body(message));
      EXPECT_EQ(immediateRetry ? 2 : 1, message->jmsHeaders.jmsDeliveryCount());
      EXPECT_NO_THROW(consumer->close());
      EXPECT_NO_THROW(session.createTextMessage("service inside callback"));
      --active;
      second.set_value();
    });
    other->setMessageListener([&](Message::Ptr) {
      const auto concurrent = active.fetch_add(1);
      EXPECT_EQ(0, concurrent);
      --active;
      sideDelivery.set_value();
    });
    send(sender, *producer, "first");
    auto firstCalled = began.get_future();
    EXPECT_EQ(std::future_status::timeout, firstCalled.wait_for(15ms));
    EXPECT_THROW(other->receiveNoWait(), IllegalStateException);
    connection.start();
    if (firstCalled.wait_for(2s) != std::future_status::ready) {
      release.count_down();
      FAIL() << "listener did not start";
    }
    send(sender, *companion, "other", Message::NOT_PERSISTENT);
    auto stop = std::async(std::launch::async, [&] { connection.stop(); });
    EXPECT_EQ(std::future_status::timeout, stop.wait_for(15ms));
    release.count_down();
    stop.get();
    if (!immediateRetry) {
      send(sender, *producer, "second", Message::NOT_PERSISTENT);
    }
    connection.start();
    ASSERT_EQ(std::future_status::ready, second.get_future().wait_for(2s));
    ASSERT_EQ(std::future_status::ready, sideDelivery.get_future().wait_for(2s));
    connection.stop();
    other->setMessageListener({});
    if (mode == Session::CLIENT_ACKNOWLEDGE) {
      session.acknowledge();
    }
    if (mode == Session::SESSION_TRANSACTED) {
      session.commit();
    }
    session.close();
    sending.start();
    auto inspector = sender.createConsumer(queue);
    EXPECT_FALSE(inspector->receiveNoWait());
  }

  for (int operation : {0, 1, 2}) {
    Exchange exchange(directory);
    Connection connection(exchange);
    auto& session = connection.createSession();
    auto queue = session.createDestination(destination::Queue, "listener-close-" + std::to_string(operation));
    auto consumer = session.createConsumer(queue);
    auto producer = session.createProducer(queue);
    std::promise<void> entered;
    std::latch release(1);
    consumer->setMessageListener([&](Message::Ptr) {
      entered.set_value();
      release.wait();
      EXPECT_NO_THROW(session.createTextMessage("callback still owns its session"));
      EXPECT_FALSE(connection.isClosed());
    });
    connection.start();
    send(session, *producer, "block close");
    if (entered.get_future().wait_for(2s) != std::future_status::ready) {
      release.count_down();
      FAIL() << "listener did not start";
    }
    auto closing = std::async(std::launch::async, [&] {
      if (operation == 0) {
        consumer->close();
      } else if (operation == 1) {
        session.close();
      } else {
        connection.close();
      }
    });
    EXPECT_EQ(std::future_status::timeout, closing.wait_for(15ms));
    release.count_down();
    closing.get();
    EXPECT_THROW(consumer->receiveNoWait(), IllegalStateException);
  }

}

// REQ-72-4
TEST_F(DeliveryContractTest, AsyncSendBarriers) {
  for (bool transacted : {false, true}) {
    Exchange exchange(directory);
    Connection connection(exchange);
    connection.start();
    auto& session = connection.createSession(transacted ? Session::SESSION_TRANSACTED : Session::AUTO_ACKNOWLEDGE);
    auto& viewer = connection.createSession();
    auto queue = session.createDestination(destination::Queue, "async-" + std::to_string(transacted));
    auto producer = session.createProducer(queue);
    auto other = session.createProducer(queue);
    auto reader = viewer.createConsumer(queue);
    std::promise<void> entered;
    std::latch release(1);
    std::vector<int> completions;
    auto first = session.createTextMessage("first", Message::PERSISTENT);
    const auto caller = std::this_thread::get_id();
    producer->sendAsync(first, [&](const SendCompletion& result) {
      EXPECT_NE(caller, std::this_thread::get_id());
      EXPECT_TRUE(result.headers);
      EXPECT_FALSE(result.error);
      if (result.headers) {
        EXPECT_EQ(startTime, result.headers->deliveryTime);
      }
      EXPECT_THROW(producer->close(), IllegalStateException);
      EXPECT_THROW(session.close(), IllegalStateException);
      EXPECT_THROW(connection.close(), IllegalStateException);
      EXPECT_THROW(session.commit(), IllegalStateException);
      EXPECT_THROW(session.rollback(), IllegalStateException);
      completions.push_back(1);
      entered.set_value();
      release.wait();
      throw std::runtime_error("ignored by completion executor");
    });
    send(session, *other, "sync", Message::PERSISTENT);
    auto last = session.createTextMessage("last", Message::PERSISTENT);
    producer->sendAsync(last, [&](const SendCompletion& result) {
      EXPECT_TRUE(result.headers);
      completions.push_back(2);
    });
    {
      Injection inject("TINY_MQ_TEST_SEND_ERROR");
      other->sendAsync(session.createTextMessage("late"), [&](const SendCompletion& result) {
        EXPECT_FALSE(result.headers);
        EXPECT_TRUE(result.error);
        completions.push_back(3);
      });
    }
    EXPECT_THROW(other->sendAsync(last, {Message::PERSISTENT, -1, 0, 0},
                                 [&](const auto&) { ADD_FAILURE() << "unexpected callback"; }),
                 std::invalid_argument);
    if (entered.get_future().wait_for(2s) != std::future_status::ready) {
      release.count_down();
      FAIL() << "completion did not start";
    }

    auto barrier = std::async(std::launch::async, [&] {
      if (transacted) {
        session.commit();
      } else {
        producer->close();
      }
    });
    EXPECT_EQ(std::future_status::timeout, barrier.wait_for(15ms));
    if (transacted) {
      EXPECT_FALSE(reader->receiveNoWait());
    }
    release.count_down();
    barrier.get();
    other->close();
    EXPECT_EQ((std::vector<int>{1, 2, 3}), completions);
    EXPECT_EQ("first", body(reader->receiveNoWait()));
    EXPECT_EQ("sync", body(reader->receiveNoWait()));
    EXPECT_EQ("last", body(reader->receiveNoWait()));
    EXPECT_FALSE(reader->receiveNoWait());

    auto pending = session.createProducer(queue);
    std::promise<void> holding;
    std::latch releaseAgain(1);
    pending->sendAsync(session.createTextMessage("barrier"), [&](const SendCompletion&) {
      holding.set_value();
      releaseAgain.wait();
    });
    if (holding.get_future().wait_for(2s) != std::future_status::ready) {
      releaseAgain.count_down();
      FAIL() << "second completion did not start";
    }
    auto closing = std::async(std::launch::async, [&] {
      if (transacted) {
        session.rollback();
      } else {
        session.close();
      }
    });
    EXPECT_EQ(std::future_status::timeout, closing.wait_for(15ms));
    releaseAgain.count_down();
    closing.get();
    if (transacted) {
      EXPECT_FALSE(reader->receiveNoWait());
    } else {
      EXPECT_EQ("barrier", body(reader->receiveNoWait()));
    }
    auto finalProducer = viewer.createProducer(queue);
    std::promise<void> finalEntered;
    std::latch finalRelease(1);
    finalProducer->sendAsync(viewer.createTextMessage("connection barrier"), [&](const SendCompletion&) {
      finalEntered.set_value();
      finalRelease.wait();
    });
    if (finalEntered.get_future().wait_for(2s) != std::future_status::ready) {
      finalRelease.count_down();
      FAIL() << "connection completion did not start";
    }
    auto connectionClose = std::async(std::launch::async, [&] { connection.close(); });
    EXPECT_EQ(std::future_status::timeout, connectionClose.wait_for(15ms));
    finalRelease.count_down();
    connectionClose.get();
    EXPECT_TRUE(connection.isClosed());
  }
}

// REQ-72-5
TEST_F(DeliveryContractTest, AcknowledgeAndRecoverMatrix) {
  for (auto mode : {Session::AUTO_ACKNOWLEDGE, Session::DUPS_OK_ACKNOWLEDGE,
                   Session::CLIENT_ACKNOWLEDGE, Session::SESSION_TRANSACTED}) {
    for (auto reliability : {Message::PERSISTENT, Message::NOT_PERSISTENT}) {
      SCOPED_TRACE(testing::Message() << mode << "/" << reliability);
      DeliveryClock::setTestTime(startTime);
      Exchange exchange(directory);
      Connection connection(exchange);
      connection.start();
      auto& session = connection.createSession(mode);
      auto& sender = connection.createSession();
      auto queue = sender.createDestination(destination::Queue, "ack-" + std::to_string(mode));
      auto otherQueue = sender.createDestination(destination::Queue, "out-of-session-" + std::to_string(mode));
      auto producer = sender.createProducer(queue);
      auto otherProducer = sender.createProducer(otherQueue);
      auto left = session.createConsumer(queue);
      auto right = session.createConsumer(queue);
      auto other = sender.createConsumer(otherQueue);
      send(sender, *producer, "one", reliability);
      send(sender, *producer, "two", reliability);
      send(sender, *producer, "three", reliability);
      send(sender, *otherProducer, "already acknowledged", reliability);
      auto expiring = sender.createTextMessage("expires");
      producer->send(expiring, {reliability, 4, 10, 0});

      const auto incompatible = left->receiveChecked([](const Message&) { return false; }, 0);
      EXPECT_EQ(ReceiveBodyDisposition::incompatible, incompatible.disposition);
      EXPECT_FALSE(incompatible.message);
      EXPECT_EQ("already acknowledged", body(other->receiveNoWait()));
      const bool trackedMismatch = mode == Session::CLIENT_ACKNOWLEDGE || mode == Session::SESSION_TRANSACTED;
      auto second = right->receiveNoWait();
      ASSERT_TRUE(second);
      EXPECT_EQ(trackedMismatch ? "two" : "one", body(second));
      EXPECT_EQ(1, second->jmsHeaders.jmsDeliveryCount());
      if (!trackedMismatch) {
        EXPECT_EQ("two", body(left->receiveNoWait()));
      }
      EXPECT_EQ("three", body(left->receiveNoWait()));
      EXPECT_EQ("expires", body(right->receiveNoWait()));
      left->close();
      DeliveryClock::setTestTime(startTime + 10);
      producer->send(sender.createTextMessage("priority"), {reliability, 9, 0, 0});

      if (mode == Session::SESSION_TRANSACTED) {
        EXPECT_THROW(session.recover(), IllegalStateException);
        session.rollback();
      } else {
        session.recover();
      }
      EXPECT_EQ("priority", body(right->receiveNoWait()));
      if (trackedMismatch || mode == Session::DUPS_OK_ACKNOWLEDGE) {
        if (trackedMismatch) {
          auto first = right->receiveNoWait();
          ASSERT_TRUE(first);
          EXPECT_EQ("one", body(first));
          EXPECT_TRUE(first->jmsHeaders.redelivered);
          EXPECT_EQ(2, first->jmsHeaders.jmsDeliveryCount());
        }
        EXPECT_EQ(trackedMismatch ? "two" : "one", body(right->receiveNoWait()));
        if (trackedMismatch) {
          EXPECT_EQ("three", body(right->receiveNoWait()));
        }
      }
      EXPECT_FALSE(right->receiveNoWait());
      EXPECT_FALSE(other->receiveNoWait());
      if (mode == Session::CLIENT_ACKNOWLEDGE) {
        session.acknowledge();
      }
      if (mode == Session::SESSION_TRANSACTED) {
        session.commit();
      }
    }
  }
  {
    Exchange exchange(directory);
    Connection connection(exchange);
    connection.start();
    auto& first = connection.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto& second = connection.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto queue = first.createDestination(destination::Queue, "recovery-bands");
    auto producer = first.createProducer(queue);
    auto left = first.createConsumer(queue);
    auto right = first.createConsumer(queue);
    auto foreign = second.createConsumer(queue);
    producer->send(first.createTextMessage("low"), {Message::PERSISTENT, 1, 0, 0});
    const auto accepted = left->receiveChecked([](const Message& message) {
      return message.type() == Message::TEXT_MESSAGE;
    }, 0);
    EXPECT_EQ(ReceiveBodyDisposition::compatible, accepted.disposition);
    EXPECT_EQ("low", body(accepted.message));
    producer->send(first.createTextMessage("high"), {Message::PERSISTENT, 8, 0, 0});
    EXPECT_EQ("high", body(right->receiveNoWait()));
    send(first, *producer, "foreign");
    EXPECT_EQ("foreign", body(foreign->receiveNoWait()));
    first.recover();
    producer->send(first.createTextMessage("new priority"), {Message::NOT_PERSISTENT, 9, 0, 0});
    EXPECT_EQ("new priority", body(right->receiveNoWait()));
    EXPECT_EQ("low", body(left->receiveNoWait()));
    EXPECT_EQ("high", body(right->receiveNoWait()));
    left->close();
    first.acknowledge();
    first.recover();
    EXPECT_FALSE(right->receiveNoWait());
    second.recover();
    auto foreignRetry = foreign->receiveNoWait();
    ASSERT_TRUE(foreignRetry);
    EXPECT_EQ("foreign", body(foreignRetry));
    EXPECT_EQ(2, foreignRetry->jmsHeaders.jmsDeliveryCount());
    second.acknowledge();
  }
  Message::Headers headers;
  headers.redelivered = true;
  EXPECT_EQ(2, headers.jmsDeliveryCount());
  headers.deliveryCount = INT32_MAX;
  EXPECT_EQ(INT32_MAX, headers.jmsDeliveryCount());
}

struct TransactionFixture {
  Exchange exchange;
  Connection connection{exchange};
  Session& normal;
  Session& transaction;
  Destination::Ptr input, output, topic;
  Consumer::Ptr first, second;

  explicit TransactionFixture(const std::string& directory)
      : exchange(directory), normal(connection.createSession()), transaction(connection.createSession(Session::SESSION_TRANSACTED)),
        input(normal.createDestination(destination::Queue, "input")),
        output(normal.createDestination(destination::Queue, "output")),
        topic(normal.createDestination(destination::Topic, "fan-out")),
        first(normal.createSharedDurableConsumer(topic, "first")),
        second(normal.createSharedDurableConsumer(topic, "second", "copies = 2")) {
    connection.start();
  }

  void stage(Message::Reliability inMode, Message::Reliability outMode,
             Message::Reliability fanoutMode, bool seed) {
    if (seed) {
      send(normal, *normal.createProducer(input), "input", inMode);
    }
    auto consumer = transaction.createConsumer(input);
    require(body(consumer->receiveNoWait()) == "input", "transaction cannot claim input");
    consumer->close();
    auto producer = transaction.createProducer(output);
    send(transaction, *producer, "queue output", outMode);
    producer->close();
    auto published = transaction.createTextMessage("topic output", fanoutMode);
    published.setIntProperty("copies", 2);
    transaction.createProducer(topic)->send(published);
  }

  void check(bool committed, bool empty = false, bool queueOutput = true) {
    auto in = normal.createConsumer(input);
    auto out = normal.createConsumer(output);
    require(body(in->receiveNoWait()) == (!committed && !empty ? "input" : ""), "transaction input outcome");
    require(body(out->receiveNoWait()) == (committed && !empty && queueOutput ? "queue output" : ""), "transaction queue output outcome");
    for (const auto& copy : {first, second}) {
      require(body(copy->receiveNoWait()) == (committed && !empty ? "topic output" : ""), "transaction fan-out outcome");
      require(!copy->receiveNoWait(), "duplicate fan-out");
    }
    require(!in->receiveNoWait() && !out->receiveNoWait(), "duplicate input or output");
  }
};

// REQ-72-6
TEST_F(DeliveryContractTest, AtomicConsumeAndSend) {
  for (auto modes : {std::array{Message::PERSISTENT, Message::PERSISTENT, Message::PERSISTENT},
                     std::array{Message::NOT_PERSISTENT, Message::NOT_PERSISTENT, Message::NOT_PERSISTENT},
                     std::array{Message::PERSISTENT, Message::NOT_PERSISTENT, Message::PERSISTENT}}) {
    for (bool commit : {false, true}) {
      const auto path = directory + std::to_string(modes[0]) + "-" + std::to_string(modes[1]) +
                        "-" + std::to_string(commit) + "/";
      TransactionFixture broker(path);
      broker.stage(modes[0], modes[1], modes[2], true);
      auto browser = broker.normal.createQueueBrowser(broker.output);
      EXPECT_TRUE(browser.enumerate().empty());
      if (commit) {
        if (modes[0] == Message::PERSISTENT) {
          Injection inject("TINY_MQ_TEST_COMMIT_ERROR");
          EXPECT_THROW(broker.transaction.commit(), Poco::IOException);
          EXPECT_TRUE(browser.enumerate().empty());
          EXPECT_FALSE(broker.first->receiveNoWait());
          EXPECT_FALSE(broker.second->receiveNoWait());
        }
        broker.transaction.commit();
      } else {
        broker.transaction.rollback();
      }
      EXPECT_NO_THROW(broker.check(commit));
    }
  }
  {
    TransactionFixture broker(directory + "error-rollback/");
    broker.stage(Message::PERSISTENT, Message::PERSISTENT, Message::PERSISTENT, true);
    {
      Injection inject("TINY_MQ_TEST_COMMIT_ERROR");
      EXPECT_THROW(broker.transaction.commit(), Poco::IOException);
    }
    broker.transaction.rollback();
    EXPECT_NO_THROW(broker.check(false));
  }
  {
    TransactionFixture broker(directory + "commit-publication/");
    broker.stage(Message::PERSISTENT, Message::PERSISTENT, Message::PERSISTENT, true);
    auto browser = broker.normal.createQueueBrowser(broker.output);
    auto observer = broker.normal.createConsumer(broker.output);
    std::promise<void> prepared;
    std::latch release(1);
    const std::function<void()> hold = [&] {
      prepared.set_value();
      release.wait();
    };
    internal::observeCommitPreparation(&hold);
    auto committing = std::async(std::launch::async, [&] { broker.transaction.commit(); });
    const auto reached = prepared.get_future().wait_for(2s);
    EXPECT_EQ(std::future_status::ready, reached);
    EXPECT_TRUE(browser.enumerate().empty());
    EXPECT_FALSE(observer->receiveNoWait());
    EXPECT_FALSE(broker.first->receiveNoWait());
    EXPECT_FALSE(broker.second->receiveNoWait());
    release.count_down();
    internal::observeCommitPreparation(nullptr);
    committing.get();
    EXPECT_NO_THROW(broker.check(true));
  }
  for (const auto* point : {"commit:before-decision", "commit:after-decision", "commit:apply-1"}) {
    const auto path = directory + point + "/";
    ASSERT_EQ(crashCode, child(path, "tx-crash", point));
    const bool committed = std::string_view(point) != "commit:before-decision";
    if (committed) {
      ASSERT_EQ(crashCode, child(path, "tx-check-commit", "replay:apply-1"));
    }
    EXPECT_EQ(0, child(path, committed ? "tx-check-commit" : "tx-check-rollback"));
    EXPECT_EQ(0, child(path, "tx-check-empty"));
  }
  const auto mixed = directory + "mixed-crash/";
  ASSERT_EQ(crashCode, child(mixed, "tx-crash-mixed", "commit:apply-1"));
  EXPECT_EQ(0, child(mixed, "tx-check-mixed"));
  EXPECT_EQ(0, child(mixed, "tx-check-empty"));
  const auto corrupt = directory + "corrupt/";
  ASSERT_EQ(crashCode, child(corrupt, "tx-crash", "commit:after-decision"));
  std::ofstream(corrupt + "delivery-72/decision", std::ios::binary | std::ios::app).put('!');
  EXPECT_EQ(74, child(corrupt, "expect-corruption"));
#ifndef _WIN32
  for (bool linkDirectory : {false, true}) {
    SCOPED_TRACE(linkDirectory ? "delivery-72 directory symlink" : "decision file symlink");
    const auto path = directory + "linked-" + std::to_string(linkDirectory) + "/";
    ASSERT_EQ(crashCode, child(path, "tx-crash", "commit:after-decision"));
    const auto journal = fs::path(path) / "delivery-72";
    const auto outside = fs::absolute(directory + "outside-" + std::to_string(linkDirectory));
    const auto entry = linkDirectory ? journal : journal / "decision";
    const auto externalDecision = linkDirectory ? outside / "decision" : outside;
    const auto expected = readScratch(journal / "decision");
    ASSERT_FALSE(expected.empty());
    ASSERT_EQ(1, expected.front());
    fs::rename(entry, outside);
    if (linkDirectory) {
      fs::create_directory_symlink(outside, entry);
    } else {
      fs::create_symlink(outside, entry);
    }
    EXPECT_EQ(74, child(path, "tx-check-commit"));
    EXPECT_EQ(expected, readScratch(externalDecision));
    EXPECT_TRUE(fs::is_symlink(entry));
    fs::remove(entry);
    fs::rename(outside, entry);
    EXPECT_EQ(0, child(path, "tx-check-commit"));
    EXPECT_EQ(0, child(path, "tx-check-empty"));
  }
  {
    SCOPED_TRACE("decision creation through a dangling symlink");
    const auto path = directory + "dangling/";
    const auto decision = fs::path(path) / "delivery-72" / "decision";
    const auto outside = fs::absolute(directory + "must-not-create");
    // Start clean, then substitute the entry after Exchange has opened the root.
    TransactionFixture broker(path);
    broker.stage(Message::PERSISTENT, Message::PERSISTENT, Message::PERSISTENT, true);
    fs::create_directory(decision.parent_path());
    fs::create_symlink(outside, decision);
    EXPECT_THROW(broker.transaction.commit(), Poco::DataFormatException);
    EXPECT_FALSE(fs::exists(outside));
    fs::remove(decision);
    broker.transaction.commit();
    EXPECT_NO_THROW(broker.check(true));
  }
  {
    SCOPED_TRACE("decision replaced after opening");
    const auto path = directory + "swapped/";
    const auto decision = fs::path(path) / "delivery-72" / "decision";
    const auto openedDecision = decision.parent_path() / "opened-decision";
    const auto outside = fs::absolute(directory + "outside-after-open");
    const std::string sentinel(1024 * 1024 + 1, 'x');
    std::ofstream(outside, std::ios::binary) << sentinel;
    TransactionFixture broker(path);
    broker.stage(Message::PERSISTENT, Message::PERSISTENT, Message::PERSISTENT, true);
    const std::function<void()> swap = [&] {
      fs::rename(decision, openedDecision);
      fs::create_symlink(outside, decision);
    };
    internal::observeCommitPreparation(&swap);
    EXPECT_NO_THROW(broker.transaction.commit());
    internal::observeCommitPreparation(nullptr);
    EXPECT_NO_THROW(broker.check(true));
    const auto retired = readScratch(openedDecision);
    ASSERT_FALSE(retired.empty());
    EXPECT_EQ(0, retired.front());
    EXPECT_TRUE(fs::is_symlink(decision));
    EXPECT_TRUE(sentinel == readScratch(outside));
  }
  {
    SCOPED_TRACE("decision FIFO");
    const auto path = directory + "fifo/";
    const auto journal = fs::path(path) / "delivery-72";
    fs::create_directories(journal);
    ASSERT_EQ(0, ::mkfifo((journal / "decision").c_str(), 0600));
    EXPECT_EQ(74, child(path, "expect-corruption"));
  }
#endif
}

void ackCrash(const std::string& directory, std::string_view scenario) {
  Exchange exchange(directory);
  Connection connection(exchange);
  auto& session = connection.createSession();
  const auto queue = session.createDestination(destination::Queue, "ack");
  const auto topic = session.createDestination(destination::Topic, "ack-topic");
  auto durable = session.createSharedDurableConsumer(topic, "ack-durable");
  auto consumer = session.createConsumer(queue);
  connection.start();
  if (scenario == "ack-seed") {
    send(session, *session.createProducer(queue), "queue");
    send(session, *session.createProducer(topic), "durable");
    return;
  }
  if (scenario == "ack-queue") {
    require(body(consumer->receiveNoWait()) == "queue", "queue seed missing");
    return;
  }
  if (scenario == "ack-durable") {
    require(body(durable->receiveNoWait()) == "durable", "durable seed missing");
    return;
  }
  const bool expectQueueRetry = scenario == "ack-queue-retry";
  const bool expectDurableRetry = scenario == "ack-durable-retry";
  auto message = consumer->receiveNoWait();
  auto copy = durable->receiveNoWait();
  if (scenario == "ack-empty") {
    require(!message && !copy, "acknowledged delivery replayed");
  } else if (scenario == "ack-queue-absent") {
    require(!message && body(copy) == "durable" && copy->jmsHeaders.jmsDeliveryCount() == 1, "ack queue replay outcome");
  } else if (scenario == "ack-durable-absent") {
    require(!copy && body(message) == "queue" && message->jmsHeaders.jmsDeliveryCount() == 1, "ack durable replay outcome");
  } else {
    require(body(message) == "queue" && body(copy) == "durable", "missing durable delivery");
    require(message->jmsHeaders.jmsDeliveryCount() == (expectQueueRetry ? 2 : 1), "queue attempt count");
    require(copy->jmsHeaders.jmsDeliveryCount() == (expectDurableRetry ? 2 : 1), "durable attempt count");
  }
}

void retryCrash(const std::string& directory, std::string_view scenario) {
  Exchange exchange(directory);
  Connection connection(exchange);
  connection.start();
  auto& session = connection.createSession(Session::CLIENT_ACKNOWLEDGE);
  auto queue = session.createDestination(destination::Queue, "origin");
  auto dlq = session.createDestination(destination::Queue, "dead");
  auto consumer = session.createConsumer(queue);
  auto dead = session.createConsumer(dlq);
  if (scenario == "retry-crash") {
    queue->setRedeliveryPolicy({-1, 50, 500, dlq});
    send(session, *session.createProducer(queue), "retry");
    require(consumer->receiveNoWait() != nullptr, "cannot claim retry input");
    session.recover();
    return;
  }
  if (scenario == "retry-check") {
    queue->setRedeliveryPolicy({0, 999, 999, {}});
    require(!consumer->receiveNoWait(), "backoff lost");
    DeliveryClock::setTestTime(startTime + 49);
    require(!consumer->receiveNoWait(), "backoff expired early");
    DeliveryClock::setTestTime(startTime + 50);
    auto next = consumer->receive(1000);
    require(body(next) == "retry" && next->jmsHeaders.jmsDeliveryCount() == 2, "retry did not survive");
    require(next->jmsHeaders.deliveryTime == startTime, "public clock changed");
    session.acknowledge();
    return;
  }
  if (scenario == "dlq-crash" || scenario == "dlq-topic-crash") {
    const bool fanout = scenario == "dlq-topic-crash";
    auto origin = fanout ? session.createDestination(destination::Topic, "origin-topic") : queue;
    origin->setRedeliveryPolicy({1, 0, 0, dlq});
    if (fanout) {
      consumer = session.createSharedDurableConsumer(origin, "origin-first");
      session.createSharedDurableConsumer(origin, "origin-second")->close();
    }
    send(session, *session.createProducer(origin), "poison");
    auto first = consumer->receiveNoWait();
    require(first && first->jmsHeaders.jmsDeliveryCount() == 1, "first poison attempt");
    session.recover();
    auto second = consumer->receiveNoWait();
    require(second && second->jmsHeaders.jmsDeliveryCount() == 2, "positive retry limit");
    session.recover();
    return;
  }
  const bool fanout = scenario.starts_with("dlq-topic-");
  if (fanout) {
    const auto topic = session.createDestination(destination::Topic, "origin-topic");
    consumer = session.createSharedDurableConsumer(topic, "origin-first");
    auto sibling = session.createSharedDurableConsumer(topic, "origin-second");
    auto copy = sibling->receiveNoWait();
    if (scenario == "dlq-topic-check") {
      require(body(copy) == "poison" && copy->jmsHeaders.jmsDeliveryCount() == 1, "independent durable attempt");
    } else {
      require(!copy, "acknowledged sibling replayed");
    }
  }
  require(!consumer->receiveNoWait(), "DLQ origin remains visible");
  const auto result = dead->receiveNoWait();
  if (scenario == "dlq-check" || scenario == "dlq-topic-check") {
    require(body(result) == "poison" && result->jmsHeaders.jmsDeliveryCount() == 3, "DLQ copy missing");
    require(result->jmsHeaders.deliveryTime == startTime, "DLQ clock changed");
    require(!dead->receiveNoWait(), "duplicate DLQ copy");
    session.acknowledge();
  } else {
    require(!result, "DLQ copy replayed after acknowledgement");
  }
}

// REQ-72-7
TEST_F(DeliveryContractTest, CrashAndDeadLetterMatrix) {
  for (const auto* kind : {"queue", "durable"}) {
    for (bool ack : {false, true}) {
      const auto path = directory + kind + std::to_string(ack) + "/";
      EXPECT_EQ(0, child(path, "ack-seed"));
      EXPECT_EQ(crashCode, child(path, std::string("ack-") + kind, ack ? "ack:after-ack" : "ack:before-ack"));
      EXPECT_EQ(0, child(path, std::string("ack-") + kind + (ack ? "-absent" : "-retry")));
      EXPECT_EQ(0, child(path, "ack-empty"));
    }
  }
  const auto retry = directory + "retry/";
  EXPECT_EQ(crashCode, child(retry, "retry-crash", "retry:scheduled"));
  EXPECT_EQ(0, child(retry, "retry-check"));

#ifndef _WIN32
  for (bool linkDirectory : {false, true}) {
    SCOPED_TRACE(linkDirectory ? "attempt-72 directory symlink" : "attempt file symlink");
    const auto path = directory + "linked-retry-" + std::to_string(linkDirectory) + "/";
    ASSERT_EQ(crashCode, child(path, "retry-crash", "retry:scheduled"));
    const auto attempts = retryAttempts(path);
    const auto sidecar = fs::directory_iterator(attempts)->path();
    const auto outside = fs::absolute(directory + "outside-retry-" + std::to_string(linkDirectory));
    const auto entry = linkDirectory ? attempts : sidecar;
    const auto externalFrame = linkDirectory ? outside / sidecar.filename() : outside;
    const auto expected = readScratch(sidecar);
    ASSERT_TRUE(expected.starts_with("TMQ72A01"));
    fs::rename(entry, outside);
    if (linkDirectory) {
      fs::create_directory_symlink(outside, entry);
    } else {
      fs::create_symlink(outside, entry);
    }
    EXPECT_EQ(74, child(path, "retry-check"));
    ASSERT_TRUE(fs::exists(externalFrame));
    EXPECT_EQ(expected, readScratch(externalFrame));
    EXPECT_TRUE(fs::is_symlink(entry));
    fs::remove(entry);
    fs::rename(outside, entry);
    EXPECT_EQ(0, child(path, "retry-check"));
  }
  {
    SCOPED_TRACE("attempt write and cleanup with substituted paths");
    const auto path = directory + "live-attempts/";
    Exchange exchange(path);
    Connection connection(exchange);
    auto& session = connection.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto queue = session.createDestination(destination::Queue, "origin");
    queue->setRedeliveryPolicy({3, 0, 0, {}});
    auto producer = session.createProducer(queue);
    auto consumer = session.createConsumer(queue);
    connection.start();
    send(session, *producer, "sidecar");
    ASSERT_EQ("sidecar", body(consumer->receiveNoWait()));

    const auto attempts = retryAttempts(path);
    const auto frame = fs::directory_iterator(attempts)->path();
    const auto temporary = fs::path(frame.string() + ".tmp");
    const auto outside = fs::absolute(directory + "outside-temporary");
    const std::string sentinel = "external scratch";
    std::ofstream(outside, std::ios::binary) << sentinel;
    fs::remove(frame);
    fs::create_symlink(outside, frame);
    fs::create_symlink(outside, temporary);
    session.recover();
    EXPECT_FALSE(fs::is_symlink(temporary));
    EXPECT_FALSE(fs::is_symlink(frame));
    EXPECT_EQ(sentinel, readScratch(outside));
    ASSERT_EQ("sidecar", body(consumer->receiveNoWait()));

    const auto externalDirectory = fs::absolute(directory + "outside-live-attempts");
    const auto externalFrame = externalDirectory / frame.filename();
    const auto expected = readScratch(frame);
    fs::rename(attempts, externalDirectory);
    fs::create_directory_symlink(externalDirectory, attempts);
    send(session, *producer, "new sidecar");
    EXPECT_THROW(consumer->receiveNoWait(), Poco::DataFormatException);
    session.acknowledge();
    ASSERT_TRUE(fs::exists(externalFrame));
    EXPECT_EQ(expected, readScratch(externalFrame));
    EXPECT_EQ(1, std::distance(fs::directory_iterator(externalDirectory), fs::directory_iterator{}));
    EXPECT_TRUE(fs::is_symlink(attempts));
    fs::remove(attempts);
    fs::rename(externalDirectory, attempts);
    ASSERT_EQ("new sidecar", body(consumer->receiveNoWait()));
    session.acknowledge();
    EXPECT_FALSE(consumer->receiveNoWait());
  }
#endif

  for (const auto* scenario : {"dlq-crash", "dlq-topic-crash"}) {
    for (const auto* point : {"dlq:before-decision", "dlq:after-decision", "dlq:apply-1"}) {
      const auto path = directory + scenario + "/" + point + "/";
      const bool fanout = std::string_view(scenario) == "dlq-topic-crash";
      const auto* check = fanout ? "dlq-topic-check" : "dlq-check";
      EXPECT_EQ(crashCode, child(path, scenario, point));
      if (std::string_view(point) != "dlq:before-decision") {
        EXPECT_EQ(crashCode, child(path, check, "replay:apply-1"));
      }
      EXPECT_EQ(0, child(path, check));
      EXPECT_EQ(0, child(path, fanout ? "dlq-topic-empty" : "dlq-empty"));
    }
  }

  legacyRecord(directory, "expired-legacy", startTime);
  Exchange exchange(directory);
  Connection connection(exchange);
  connection.start();
  auto& session = connection.createSession(Session::CLIENT_ACKNOWLEDGE);
  auto expired = session.createConsumer(session.createDestination(destination::Queue, "expired-legacy"));
  EXPECT_FALSE(expired->receiveNoWait());
  const auto unlimited = session.createDestination(destination::Queue, "unlimited");
  auto publisher = session.createProducer(unlimited);
  auto reader = session.createConsumer(unlimited);
  for (auto reliability : {Message::PERSISTENT, Message::NOT_PERSISTENT}) {
    send(session, *publisher, "unlimited", reliability);
    for (int32_t attempt = 1; attempt <= 10; ++attempt) {
      auto message = reader->receiveNoWait();
      ASSERT_TRUE(message);
      EXPECT_EQ(attempt, message->jmsHeaders.jmsDeliveryCount());
      EXPECT_EQ(startTime, message->jmsHeaders.deliveryTime);
      if (attempt < 10) {
        session.recover();
      }
    }
    session.acknowledge();
  }
  unlimited->setRedeliveryPolicy({0, 0, 0, {}});
  send(session, *publisher, "explicit drop");
  ASSERT_TRUE(reader->receiveNoWait());
  session.recover();
  EXPECT_FALSE(reader->receiveNoWait());
}

// REQ-72-8
TEST_F(DeliveryContractTest, SessionSendOrder) {
  for (auto reliability : {Message::PERSISTENT, Message::NOT_PERSISTENT}) {
    for (bool transacted : {false, true}) {
      DeliveryClock::setTestTime(startTime);
      Exchange exchange(directory);
      Connection connection(exchange);
      connection.start();
      auto& sender = connection.createSession(transacted ? Session::SESSION_TRANSACTED : Session::AUTO_ACKNOWLEDGE);
      auto& receiver = connection.createSession();
      const auto queue = sender.createDestination(destination::Queue, "order-" + std::to_string(reliability));
      auto first = sender.createProducer(queue);
      auto second = sender.createProducer(queue);
      auto consumer = receiver.createConsumer(queue, "include = TRUE");
      auto topic = sender.createDestination(destination::Topic, "order-topic");
      auto firstSub = receiver.createSharedDurableConsumer(topic, "first");
      auto secondSub = receiver.createSharedDurableConsumer(topic, "second", "include = TRUE");
      auto topicOne = sender.createProducer(topic);
      auto topicTwo = sender.createProducer(topic);
      const auto message = [&](const char* value, bool include = true) {
        auto result = sender.createTextMessage(value, reliability);
        result.setBoolProperty("include", include);
        return result;
      };
      first->send(message("one"));
      second->sendAsync(message("two"), [](const SendCompletion& completion) { EXPECT_FALSE(completion.error); });
      first->send(message("skip", false));
      second->send(message("three"));
      first->send(message("priority"), {reliability, 9, 0, 0});
      second->send(message("delay"), {reliability, 9, 0, 10});
      topicOne->send(message("topic-one"));
      topicTwo->send(message("topic-two"));
      if (transacted) {
        EXPECT_FALSE(consumer->receiveNoWait());
        sender.commit();
      }
      for (const auto* expected : {"priority", "one", "two", "three"}) {
        EXPECT_EQ(expected, body(consumer->receiveNoWait()));
      }
      EXPECT_FALSE(consumer->receiveNoWait());
      for (const auto& sub : {firstSub, secondSub}) {
        EXPECT_EQ("topic-one", body(sub->receiveNoWait()));
        EXPECT_EQ("topic-two", body(sub->receiveNoWait()));
        EXPECT_FALSE(sub->receiveNoWait());
      }
      DeliveryClock::setTestTime(startTime + 10);
      EXPECT_EQ("delay", body(consumer->receive(1000)));
      auto skipped = receiver.createConsumer(queue);
      EXPECT_EQ("skip", body(skipped->receiveNoWait()));
    }
  }
}
}  // namespace

void setDeliveryTestExecutable(const char* invocation) { executable = fs::absolute(invocation).string(); }

void prepareDeliveryReplayBenchmark(const std::string& directory) {
  require(child(directory + "/", "tx-crash", "commit:after-decision") == crashCode,
          "cannot prepare journal replay benchmark");
}

int deliveryTestChild(const char* directory, const char* name) {
  DeliveryClock::setTestTime(startTime);
  const std::string_view scenario(name);
  try {
    if (scenario == "expect-corruption") {
      Exchange exchange(directory);
      return 1;
    }
    if (scenario.starts_with("tx-")) {
      TransactionFixture broker(directory);
      if (scenario == "tx-crash" || scenario == "tx-crash-mixed") {
        const auto output = scenario == "tx-crash" ? Message::PERSISTENT : Message::NOT_PERSISTENT;
        broker.stage(Message::PERSISTENT, output, Message::PERSISTENT, true);
        broker.transaction.commit();
        return 1;
      }

      const bool mixed = scenario == "tx-check-mixed";
      broker.check(scenario == "tx-check-commit" || mixed, scenario == "tx-check-empty", !mixed);
    } else if (scenario.starts_with("ack-")) {
      ackCrash(directory, scenario);
    } else {
      retryCrash(directory, scenario);
    }
    return 0;
  } catch (const Poco::DataFormatException& error) {
    std::cerr << error.displayText() << std::endl;
    return 74;
  } catch (const std::exception& error) {
    std::cerr << "child " << name << ": " << error.what() << std::endl;
    return 1;
  }
}
}  // namespace tiny_mq
