#include <gtest/gtest.h>

#include "Connection.h"
#include "DestinationHash.h"
#include "Exceptions.h"
#include "TestHelper.h"
#include <Poco/Timestamp.h>
#include <algorithm>
#include <array>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>

namespace tiny_mq {
namespace {
class DestinationContractTest : public ::testing::Test {
 protected:
  const std::string path = CurrentTestSuiteStorageDir();
  void SetUp() override { RemoveTestStorageDir(path); }
  void TearDown() override { RemoveTestStorageDir(path); }
};

void row(const char* requirement, const char* label) {
  std::cout << '[' << requirement << "] " << label << '\n';
}

TextMessage::Ptr text(const Consumer::Ptr& consumer, int64_t timeout = 0) {
  return Message::As<TextMessage>(consumer->recv(timeout));
}

void expectText(const Consumer::Ptr& consumer, std::string_view value, int64_t timeout = 0) {
  auto message = text(consumer, timeout);
  ASSERT_NE(message, nullptr);
  EXPECT_EQ(message->text(), value);
}

TEST_F(DestinationContractTest, SelectiveQueueClaim) {
  {
    Exchange exchange(path);
    Connection writer(exchange), left(exchange), right(exchange);
    writer.start();
    left.start();
    right.start();
    auto& send = writer.createSession();
    auto& a = left.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto& b = right.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto queue = send.createDestination(destination::Queue, "claims");
    auto producer = send.createProducer(queue);
    auto one = a.createConsumer(queue, "rank < 3");
    auto two = b.createConsumer(queue, "rank > 1");
    auto emit = [&](std::string value, int rank, Message::Reliability reliability) {
      auto message = send.createTextMessage(std::move(value), reliability);
      message.setIntProperty("rank", rank);
      producer->send(message);
    };

    row("REQ-71-1", "overlapping selectors, two independent concurrent sessions, mixed persistence");
    constexpr int count = 100;
    std::barrier start(3);
    std::vector<std::string> results[2];
    auto claim = [&](int index, const Consumer::Ptr& consumer) {
      start.arrive_and_wait();
      for (int i = 0; i < count / 2; ++i) {
        auto message = text(consumer, 2'000'000);
        if (!message) {
          return;
        }
        results[index].push_back(message->text());
        consumer->acknowledgeOn(*message);
      }
    };
    std::jthread first(claim, 0, one);
    std::jthread second(claim, 1, two);
    start.arrive_and_wait();
    for (int i = 0; i < count; ++i) {
      emit(std::to_string(i), 2, i % 2 ? Message::PERSISTENT : Message::NOT_PERSISTENT);
    }
    first.join();
    second.join();
    ASSERT_EQ(results[0].size(), count / 2);
    ASSERT_EQ(results[1].size(), count / 2);
    auto combined = results[0];
    combined.insert(combined.end(), results[1].begin(), results[1].end());
    std::sort(combined.begin(), combined.end());
    EXPECT_EQ(std::adjacent_find(combined.begin(), combined.end()), combined.end());

    row("REQ-71-1", "unmatched remains available, closed consumer cannot claim, live session can ack");
    emit("unmatched", 4, Message::PERSISTENT);
    emit("pending", 2, Message::PERSISTENT);
    auto pending = text(one);
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(pending->text(), "pending");
    a.deleteConsumer(one->id());
    EXPECT_THROW(one->recv(0), IllegalStateException);
    expectText(two, "unmatched");
    EXPECT_EQ(two->recv(0), nullptr);
    one->acknowledgeOn(*pending);

    row("REQ-71-1", "recover reopens only the owning session claims, old handle cannot ack next attempt");
    emit("recover", 2, Message::PERSISTENT);
    auto initial = text(two);
    ASSERT_NE(initial, nullptr);
    a.recover();
    EXPECT_EQ(two->recv(0), nullptr);
    b.recover();
    auto recovered = text(two);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->text(), "unmatched");
    EXPECT_TRUE(recovered->jmsHeaders.redelivered);
    two->acknowledgeOn(*recovered);
    recovered = text(two);
    ASSERT_NE(recovered, nullptr);
    EXPECT_EQ(recovered->text(), "recover");
    two->acknowledgeOn(*initial);

    row("REQ-71-1", "transacted claim still held after consumer close, rollback reopens it");
    Connection transaction(exchange);
    transaction.start();
    auto& tx = transaction.createSession(Session::SESSION_TRANSACTED);
    auto txConsumer = tx.createConsumer(queue, "rank = 1");
    emit("rollback", 1, Message::NOT_PERSISTENT);
    expectText(txConsumer, "rollback");
    tx.deleteConsumer(txConsumer->id());
    auto receiver = a.createConsumer(queue);
    EXPECT_EQ(receiver->recv(0), nullptr);
    tx.rollback();
    expectText(receiver, "rollback");

    row("REQ-71-1", "session close reopens an unconfirmed claim from a closed consumer");
    b.deleteConsumer(two->id());
    EXPECT_EQ(receiver->recv(0), nullptr);
    right.close();
    expectText(receiver, "recover");
    emit("confirmed", 2, Message::PERSISTENT);
    auto autoAck = send.createConsumer(queue);
    expectText(autoAck, "confirmed");
  }
  {
    row("REQ-71-1", "restart replays only unconfirmed persistent messages");
    Exchange exchange(path);
    Connection connection(exchange);
    connection.start();
    auto& session = connection.createSession();
    auto receiver = session.createConsumer(session.createDestination(destination::Queue, "claims"));
    expectText(receiver, "recover");
    EXPECT_EQ(receiver->recv(0), nullptr);
  }
}

TEST_F(DestinationContractTest, SubscriptionFanout) {
  {
    Exchange exchange(path);
    Connection sender(exchange), subscribers(exchange);
    sender.start();
    subscribers.start();
    auto& tx = sender.createSession(Session::SESSION_TRANSACTED);
    auto& plain = sender.createSession();
    auto& a = subscribers.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto& b = subscribers.createSession();
    auto topic = a.createDestination(destination::Topic, "fanout");
    auto producer = tx.createProducer(topic);
    auto immediate = plain.createProducer(topic);
    auto unshared = a.createConsumer(topic, "eligible = TRUE");
    auto sharedA = a.createSharedConsumer(topic, "shared");
    auto sharedB = b.createSharedConsumer(topic, "shared");
    auto offline = a.createSharedDurableConsumer(topic, "offline");
    a.deleteConsumer(offline->id());

    row("REQ-71-2", "persistent and non-persistent send and commit, independent copies and one shared claim");
    for (auto reliability : {Message::NOT_PERSISTENT, Message::PERSISTENT}) {
      for (bool commit : {false, true}) {
        const auto label = std::string(commit ? "commit" : "send") +
            (reliability == Message::PERSISTENT ? " persistent" : " non-persistent");
        row("REQ-71-2", label.c_str());
        auto message = tx.createTextMessage(label, reliability);
        message.setBoolProperty("eligible", true);
        if (commit) {
          producer->send(message);
          EXPECT_EQ(sharedA->recv(0), nullptr);
          tx.commit();
        } else {
          immediate->send(message);
        }
        auto own = text(unshared);
        ASSERT_NE(own, nullptr);
        EXPECT_EQ(own->text(), label);
        unshared->acknowledgeOn(*own);
        auto shared = text(sharedA);
        ASSERT_NE(shared, nullptr);
        EXPECT_EQ(shared->text(), label);
        EXPECT_NE(own, shared);
        EXPECT_EQ(sharedB->recv(0), nullptr);
        sharedA->acknowledgeOn(*shared);
      }
    }

    row("REQ-71-2", "commit chooses subscriptions and selector; delayed copy retains publication membership");
    for (auto reliability : {Message::NOT_PERSISTENT, Message::PERSISTENT}) {
      for (bool commit : {false, true}) {
        const auto label = std::string(commit ? "scheduled commit" : "scheduled send") +
            (reliability == Message::PERSISTENT ? " persistent" : " non-persistent");
        row("REQ-71-2", label.c_str());
        const auto opts = SendOptions{reliability, 4, 0, 120};
        auto delayed = tx.createTextMessage(label);
        if (commit) {
          producer->send(delayed, opts);
        }
        auto atPublication = a.createConsumer(topic);
        if (commit) {
          tx.commit();
        } else {
          immediate->send(delayed, opts);
        }
        auto tooLate = a.createConsumer(topic);
        EXPECT_EQ(unshared->recv(0), nullptr);
        EXPECT_EQ(atPublication->recv(0), nullptr);
        expectText(atPublication, label, 1'000'000);
        expectText(sharedB, label, 1'000'000);
        EXPECT_EQ(tooLate->recv(0), nullptr);
        a.deleteConsumer(atPublication->id());
        a.deleteConsumer(tooLate->id());
      }
    }

    row("REQ-71-2", "last shared nondurable close discards ready and scheduled backlog");
    immediate->send(a.createTextMessage("discard ready"));
    immediate->send(a.createTextMessage("discard delayed"), {Message::NOT_PERSISTENT, 4, 0, 120});
    a.deleteConsumer(sharedA->id());
    b.deleteConsumer(sharedB->id());
    auto recreated = b.createSharedConsumer(topic, "shared");
    EXPECT_EQ(recreated->recv(250'000), nullptr);
  }
  {
    row("REQ-71-2", "offline durable receives persistent copies across restart and commits before reconnect");
    Exchange exchange(path);
    Connection connection(exchange);
    connection.start();
    auto& tx = connection.createSession(Session::SESSION_TRANSACTED);
    auto& session = connection.createSession();
    auto topic = tx.createDestination(destination::Topic, "fanout");
    auto producer = tx.createProducer(topic);
    producer->send(tx.createTextMessage("post-restart commit", Message::PERSISTENT));
    tx.commit();
    auto one = session.createSharedDurableConsumer(topic, "offline");
    auto two = tx.createSharedDurableConsumer(topic, "offline");
    expectText(one, "send persistent");
    expectText(two, "commit persistent");
    tx.commit();
    expectText(one, "scheduled send persistent");
    expectText(one, "scheduled commit persistent");
    expectText(one, "post-restart commit");
    EXPECT_EQ(two->recv(0), nullptr);
  }
  {
    Exchange exchange(path);
    Connection sender(exchange), subscribers(exchange);
    sender.start();
    subscribers.start();
    auto& send = sender.createSession();
    auto& a = subscribers.createSession();
    auto& b = subscribers.createSession();
    for (bool durable : {false, true}) {
      const char* name = durable ? "parallel-durable" : "parallel-nondurable";
      SCOPED_TRACE(name);
      row("REQ-71-2", durable ? "shared durable concurrent claim yields every publication exactly once"
                             : "shared nondurable concurrent claim yields every publication exactly once");
      const auto topic = send.createDestination(destination::Topic, name);
      const auto producer = send.createProducer(topic);
      auto subscribe = [&](Session& session) {
        return durable ? session.createSharedDurableConsumer(topic, name) : session.createSharedConsumer(topic, name);
      };
      const auto one = subscribe(a);
      const auto two = subscribe(b);

      constexpr int rounds = 25;
      std::barrier start(3);
      std::array<std::vector<std::string>, 2> received;
      auto claim = [&](const Consumer::Ptr& consumer, std::vector<std::string>& results) {
        for (int round = 0; round < rounds; ++round) {
          start.arrive_and_wait();
          if (auto message = text(consumer, 2'000'000)) {
            results.push_back(message->text());
          }
        }
      };
      std::jthread first([&] { claim(one, received[0]); });
      std::jthread second([&] { claim(two, received[1]); });
      std::vector<std::string> expected;
      for (int round = 0; round < rounds; ++round) {
        start.arrive_and_wait();
        for (auto reliability : {Message::NOT_PERSISTENT, Message::PERSISTENT}) {
          const auto& value = expected.emplace_back("publication-" + std::to_string(expected.size()));
          producer->send(send.createTextMessage(value, reliability));
        }
      }
      first.join();
      second.join();
      ASSERT_EQ(received[0].size(), rounds);
      ASSERT_EQ(received[1].size(), rounds);
      auto combined = received[0];
      combined.insert(combined.end(), received[1].begin(), received[1].end());
      std::sort(combined.begin(), combined.end());
      std::sort(expected.begin(), expected.end());
      EXPECT_EQ(combined, expected);
      EXPECT_EQ(one->recv(0), nullptr);
      EXPECT_EQ(two->recv(0), nullptr);
    }
  }
}

TEST_F(DestinationContractTest, DurableIdentityMatrix) {
  namespace fs = std::filesystem;
  const auto registry = fs::path(path) / "subscriptions-71";
  const auto saved = fs::path(path) / "old-generation";
  const auto preSwapRoot = fs::path(path) / "pre-swap";
  const auto preSwapRegistry = preSwapRoot / registry.filename();
  fs::path original;
  {
    Exchange exchange(path);
    Connection identified(exchange), anonymous(exchange);
    anonymous.start();
    identified.setClientID("client/with\x1f" "separator");
    identified.start();
    auto& session = identified.createSession(Session::CLIENT_ACKNOWLEDGE);
    auto& noId = anonymous.createSession();
    auto one = session.createDestination(destination::Topic, "first");
    auto two = session.createDestination(destination::Topic, "second");
    auto send = noId.createProducer(one);

    row("REQ-71-3", "shared names without client ID; unshared without client ID rejected");
    EXPECT_THROW(noId.createDurableConsumer(one, "requires-ID", "", false), IllegalStateException);

    row("REQ-71-3", "active unshared definitions, cross-topic replacement and shared kind conflict rejected");
    auto consumer = session.createDurableConsumer(one, "name", "rank = 1", false);
    EXPECT_THROW(session.createDurableConsumer(one, "name", "rank = 1", false), IllegalStateException);
    EXPECT_THROW(session.createDurableConsumer(two, "name", "rank = 1", false), IllegalStateException);
    EXPECT_THROW(session.createDurableConsumer(one, "name", "rank = 1", true), IllegalStateException);
    EXPECT_THROW(session.createSharedDurableConsumer(one, "name"), IllegalStateException);
    EXPECT_THROW(session.unsubscribe("name"), IllegalStateException);
    auto otherSpace = session.createSharedConsumer(one, "name", "rank = 1");
    EXPECT_THROW(session.createSharedConsumer(two, "name", "rank = 1"), IllegalStateException);
    EXPECT_THROW(session.createSharedConsumer(one, "name", "rank = 2"), IllegalStateException);
    session.deleteConsumer(otherSpace->id());

    auto emit = [&](const char* description, int rank = 1) {
      auto message = noId.createTextMessage(description, Message::PERSISTENT);
      message.setIntProperty("rank", rank);
      send->send(message);
    };
    emit("claimed");
    auto pending = text(consumer);
    ASSERT_NE(pending, nullptr);
    session.deleteConsumer(consumer->id());
    row("REQ-71-3", "unconfirmed claim after close forbids unsubscribe and replacement");
    EXPECT_THROW(session.createDurableConsumer(one, "name", "", true), IllegalStateException);
    EXPECT_THROW(session.unsubscribe("name"), IllegalStateException);
    consumer->acknowledgeOn(*pending);

    row("REQ-71-3", "trim-equivalent reattach retains backlog; opposite durable kind still conflicts when inactive");
    emit("equivalent");
    EXPECT_THROW(session.createSharedDurableConsumer(one, "name"), IllegalStateException);
    consumer = session.createDurableConsumer(one, "name", "  rank = 1  ", false);
    expectText(consumer, "equivalent");
    session.recover();
    session.deleteConsumer(consumer->id());

    row("REQ-71-3", "changing only the unshared durable topic replaces its generation and drops the backlog");
    for (const auto& entry : fs::directory_iterator(registry)) {
      if (entry.is_directory()) {
        original = entry.path();
      }
    }
    ASSERT_FALSE(original.empty());
    fs::copy(original, saved, fs::copy_options::recursive);
    fs::create_directory(preSwapRoot);
    fs::copy(registry, preSwapRegistry, fs::copy_options::recursive);

    auto replacement = session.createDurableConsumer(two, "name", "rank = 1", false);
    EXPECT_EQ(replacement->recv(0), nullptr);
    session.deleteConsumer(replacement->id());
    ASSERT_FALSE(fs::exists(original));
    // Preserve the actual replacement generation and metadata on both sides of the swap.
    fs::copy_file(registry / "registry", preSwapRegistry / "registry.pending");
    int newGenerations = 0;
    for (const auto& entry : fs::directory_iterator(registry)) {
      if (!entry.is_directory()) {
        continue;
      }
      fs::copy(entry.path(), preSwapRegistry / entry.path().filename(), fs::copy_options::recursive);
      ++newGenerations;
    }
    ASSERT_EQ(newGenerations, 1);
    fs::copy(saved, original, fs::copy_options::recursive);

    for (bool changeNoLocal : {false, true}) {
      row("REQ-71-3", changeNoLocal ? "changing only unshared durable noLocal drops the backlog"
                                   : "changing only the unshared durable selector drops the backlog");
      const char* name = changeNoLocal ? "replace-no-local" : "replace-selector";
      auto old = session.createDurableConsumer(one, name, "rank = 1", false);
      session.deleteConsumer(old->id());
      emit("unshared backlog");
      old = session.createDurableConsumer(one, name, "rank = 1", false);
      expectText(old, "unshared backlog");
      session.recover();
      session.deleteConsumer(old->id());
      auto changed = session.createDurableConsumer(one, name, changeNoLocal ? "rank = 1" : "rank >= 1", changeNoLocal);
      EXPECT_EQ(changed->recv(0), nullptr);
      session.deleteConsumer(changed->id());
      session.unsubscribe(name);
    }

    row("REQ-71-3", "valid punctuation, Unicode identifier parts, 128 code points");
    for (const auto& name : {std::string(".-_$¢Ⅸ‿"), std::string(128, 'a'), std::string("𐐀"), std::string("a\0b", 3)}) {
      auto valid = noId.createSharedDurableConsumer(one, name);
      noId.deleteConsumer(valid->id());
      noId.unsubscribe(name);
    }
    row("REQ-71-3", "invalid lengths, path punctuation, emoji, malformed sequence and surrogate");
    for (const auto& name : {std::string(), std::string(129, 'a'), std::string("a/b"), std::string("two words"),
                             std::string("😀"), std::string("\u1C89"), std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                             std::string("\xf0\x90")}) {
      EXPECT_THROW(noId.createSharedDurableConsumer(one, name), std::invalid_argument);
    }

    auto withId = session.createSharedDurableConsumer(one, "shared", "rank = 1");
    auto withoutId = noId.createSharedDurableConsumer(one, "shared");
    row("REQ-71-3", "active shared durable rejects a different topic with the same selector");
    EXPECT_THROW(session.createSharedDurableConsumer(two, "shared", "rank = 1"), IllegalStateException);
    row("REQ-71-3", "active shared durable rejects a different selector on the same topic");
    EXPECT_THROW(session.createSharedDurableConsumer(one, "shared", "rank >= 1"), IllegalStateException);
    emit("shared pending");
    auto sharedPending = text(withId);
    ASSERT_NE(sharedPending, nullptr);
    EXPECT_EQ(sharedPending->text(), "shared pending");
    session.deleteConsumer(withId->id());
    row("REQ-71-3", "closed shared durable with a pending claim rejects unsubscribe");
    EXPECT_THROW(session.unsubscribe("shared"), IllegalStateException);
    row("REQ-71-3", "closed shared durable with a pending claim rejects replacement");
    EXPECT_THROW(session.createSharedDurableConsumer(one, "shared", "rank >= 1"), IllegalStateException);
    withId->acknowledgeOn(*sharedPending);

    row("REQ-71-3", "trim-equivalent shared durable selector retains the backlog");
    emit("shared backlog");
    withId = session.createSharedDurableConsumer(one, "shared", "  rank = 1  ");
    expectText(withId, "shared backlog");
    session.recover();
    session.deleteConsumer(withId->id());
    row("REQ-71-3", "permitted shared durable selector replacement drops backlog and accepts new matches");
    withId = session.createSharedDurableConsumer(one, "shared", "rank >= 1");
    EXPECT_EQ(withId->recv(0), nullptr);
    emit("shared replacement", 2);
    auto fresh = text(withId);
    ASSERT_NE(fresh, nullptr);
    EXPECT_EQ(fresh->text(), "shared replacement");
    withId->acknowledgeOn(*fresh);

    row("REQ-71-3", "unsubscribe shared durable both with and without client ID, rejects unknown key");
    session.deleteConsumer(withId->id());
    noId.deleteConsumer(withoutId->id());
    session.unsubscribe("shared");
    EXPECT_THROW(session.unsubscribe("shared"), std::invalid_argument);
    noId.unsubscribe("shared");
    EXPECT_THROW(noId.unsubscribe("shared"), std::invalid_argument);

    row("REQ-71-3", "legacy topic-scoped backlog at historical address and isolated from a new key");
    auto legacy = noId.createDurableConsumer(one, "same");
    noId.deleteConsumer(legacy->id());
    emit("legacy");
    const auto legacyPath = fs::path(path) / std::to_string(destination::hash(destination::Topic, "first")) / "durable-same";
    if (const char* fixture = std::getenv("TINY_MQ_LEGACY_FIXTURE")) {
      const fs::path exportRoot(fixture);
      ASSERT_TRUE(fs::is_directory(exportRoot) && fs::is_empty(exportRoot)) << exportRoot;
      const auto exportPath = exportRoot / legacyPath.lexically_relative(path);
      fs::create_directories(exportPath.parent_path());
      fs::copy(legacyPath, exportPath, fs::copy_options::recursive);
      row("REQ-71-3", "exported legacy backlog to an empty fixture root for the historical reader");
    }
    const auto legacyCopy = fs::path(path) / "legacy-backup";
    fs::copy(legacyPath, legacyCopy, fs::copy_options::recursive);
    const auto legacyId = Poco::UUIDGenerator::defaultGenerator().createFromName(
        Poco::UUID("b5c6d7e8-f9a0-4b5c-9d0e-1f2a3b4c5d6e"), "topic://first/same");
    linear_storage::Storage oldReader(legacyId, Poco::Path(legacyCopy.string()).makeDirectory());
    const auto records = oldReader.scan();
    ASSERT_EQ(records.size(), 1u);
    const auto& data = records.front().second;
    ASSERT_FALSE(data.empty());
    EXPECT_EQ(data.front(), static_cast<char>(Message::TEXT_MESSAGE));
    TextMessage recovered;
    recovered.fromBytes(BytesVector(data.begin() + 1, data.end()));
    EXPECT_EQ(recovered.text(), "legacy");

    auto standard = noId.createSharedDurableConsumer(one, "same");
    EXPECT_EQ(standard->recv(0), nullptr);
    noId.deleteConsumer(standard->id());
  }
  {
    row("REQ-71-3", "pre-swap restart: registry.pending and the new generation leave the old definition and backlog active");
    Exchange exchange(preSwapRoot.string());
    Connection identified(exchange);
    identified.setClientID("client/with\x1f" "separator");
    identified.start();
    auto& session = identified.createSession();
    auto one = session.createDestination(destination::Topic, "first");
    auto offline = session.createTextMessage("before reattach", Message::PERSISTENT);
    offline.setIntProperty("rank", 1);
    session.createProducer(one)->send(offline);
    auto restored = session.createDurableConsumer(one, "name", "rank = 1", false);
    expectText(restored, "equivalent");
    expectText(restored, "before reattach");
    EXPECT_EQ(restored->recv(0), nullptr);
  }
  {
    row("REQ-71-3", "post-swap restart: new definition stays active, orphaned backlog is unreachable and legacy reopens");
    Exchange exchange(path);
    Connection identified(exchange), anonymous(exchange);
    anonymous.start();
    identified.setClientID("client/with\x1f" "separator");
    identified.start();
    auto& session = identified.createSession();
    auto& noId = anonymous.createSession();
    auto two = session.createDestination(destination::Topic, "second");
    auto one = session.createDestination(destination::Topic, "first");
    auto offline = session.createTextMessage("after swap", Message::PERSISTENT);
    offline.setIntProperty("rank", 1);
    session.createProducer(two)->send(offline);
    auto replacement = session.createDurableConsumer(two, "name", "rank = 1", false);
    expectText(replacement, "after swap");
    EXPECT_EQ(replacement->recv(0), nullptr);
    session.deleteConsumer(replacement->id());
    session.unsubscribe("name");
    auto recreated = session.createDurableConsumer(one, "name", "rank = 1", false);
    EXPECT_EQ(recreated->recv(0), nullptr);
    EXPECT_EQ(noId.createSharedDurableConsumer(one, "same")->recv(0), nullptr);
    expectText(noId.createDurableConsumer(one, "same"), "legacy");
  }
  {
    row("REQ-71-3", "invalid metadata fails with a diagnostic and preserves existing generation");
    const auto metadata = registry / "registry";
    std::ofstream file(metadata, std::ios::app);
    file << "malformed metadata\n";
    file.close();
    EXPECT_THROW(Exchange broker(path), Poco::DataFormatException);
    EXPECT_TRUE(fs::is_directory(original));
  }
}

TEST_F(DestinationContractTest, NoLocalMatrix) {
  Exchange exchange(path);
  Connection local(exchange), remote(exchange);
  remote.start();
  local.setClientID("local");
  local.start();
  auto& publisher = local.createSession();
  auto& subscriber = local.createSession();
  auto& other = remote.createSession(Session::SESSION_TRANSACTED);
  auto topic = publisher.createDestination(destination::Topic, "nolocal");
  auto send = publisher.createProducer(topic);
  auto tx = other.createProducer(topic);
  auto privateSub = subscriber.createConsumer(topic, "", true);
  auto shared = subscriber.createSharedConsumer(topic, "shared");
  auto durable = subscriber.createDurableConsumer(topic, "offline", "", true);

  row("REQ-71-4", "same connection filtered for both unshared kinds; shared receives local publication");
  send->send(publisher.createTextMessage("local"));
  EXPECT_EQ(privateSub->recv(0), nullptr);
  EXPECT_EQ(durable->recv(0), nullptr);
  expectText(shared, "local");
  row("REQ-71-4", "unshared nondurable uses connection identity even with equal client IDs");
  {
    Connection alias(exchange);
    alias.setClientID("local");
    alias.start();
    auto& sender = alias.createSession();
    sender.createProducer(topic)->send(sender.createTextMessage("equal ID", Message::PERSISTENT));
    expectText(privateSub, "equal ID");
    expectText(shared, "equal ID");
    EXPECT_EQ(durable->recv(0), nullptr);
  }
  row("REQ-71-4", "foreign commit delivers each unshared copy");
  tx->send(other.createTextMessage("foreign", Message::PERSISTENT));
  other.commit();
  expectText(privateSub, "foreign");
  expectText(durable, "foreign");
  expectText(shared, "foreign");
  local.close();

  row("REQ-71-4", "offline durable filters another connection with same client ID");
  Connection reused(exchange);
  reused.setClientID("local");
  reused.start();
  auto& reconnect = reused.createSession();
  reconnect.createProducer(topic)->send(reconnect.createTextMessage("same ID", Message::PERSISTENT));
  tx->send(other.createTextMessage("offline foreign", Message::PERSISTENT));
  other.commit();
  auto opened = reconnect.createDurableConsumer(topic, "offline", "", true);
  expectText(opened, "offline foreign");
  EXPECT_EQ(opened->recv(0), nullptr);
  EXPECT_THROW(other.createDurableConsumer(topic, "missing", "", true), IllegalStateException);
}

TEST_F(DestinationContractTest, BrowseWithoutClaim) {
  Exchange exchange(path);
  Connection writer(exchange), reader(exchange), inspector(exchange);
  writer.start();
  reader.start();
  inspector.start();
  auto& send = writer.createSession();
  auto& receive = reader.createSession(Session::CLIENT_ACKNOWLEDGE);
  auto& browse = inspector.createSession(Session::CLIENT_ACKNOWLEDGE);
  auto queue = browse.createDestination(destination::Queue, "browse");
  auto producer = send.createProducer(queue);
  auto consumer = receive.createConsumer(queue);
  auto excluded = receive.createConsumer(queue, "selected = FALSE");
  auto browser = browse.createQueueBrowser(queue, "selected = TRUE");
  auto emit = [&](const char* value, SendOptions options, bool selected = true) {
    auto message = send.createTextMessage(value);
    message.setBoolProperty("selected", selected);
    message.jmsHeaders.correlationId = value;
    producer->send(message, options);
    return message;
  };
  auto expectSnapshot = [](const std::vector<Message::Ptr>& messages, const std::vector<const TextMessage*>& expected) {
    ASSERT_EQ(messages.size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(i);
      const auto copy = Message::As<TextMessage>(messages[i]);
      ASSERT_NE(copy, nullptr);
      const auto& sent = *expected[i];
      EXPECT_EQ(copy->text(), sent.text());
      EXPECT_EQ(copy->reliability, sent.reliability);
      EXPECT_EQ(copy->property<property::Bool>("selected"), sent.property<property::Bool>("selected"));
      EXPECT_EQ(copy->jmsHeaders.messageId, sent.jmsHeaders.messageId);
      EXPECT_EQ(copy->jmsHeaders.timestamp, sent.jmsHeaders.timestamp);
      EXPECT_EQ(copy->jmsHeaders.correlationId, sent.jmsHeaders.correlationId);
      EXPECT_EQ(copy->jmsHeaders.priority, sent.jmsHeaders.priority);
      EXPECT_EQ(copy->jmsHeaders.expiration, sent.jmsHeaders.expiration);
      EXPECT_EQ(copy->jmsHeaders.deliveryTime, sent.jmsHeaders.deliveryTime);
      EXPECT_EQ(copy->jmsHeaders.jmsDeliveryCount(), 1);
      EXPECT_FALSE(copy->jmsHeaders.redelivered);
    }
  };

  row("REQ-71-5", "snapshot excludes claimed, expired, future and unmatched; priority and FIFO stay stable");
  emit("claimed", {Message::PERSISTENT, 9});
  auto held = text(consumer);
  ASSERT_NE(held, nullptr);
  emit("unmatched", {Message::NOT_PERSISTENT, 9}, false);
  emit("expired", {Message::PERSISTENT, 9, 1});
  const auto delayed = emit("delayed", {Message::PERSISTENT, 7, 0, 600});
  const auto low = emit("low", {Message::PERSISTENT, 1});
  const auto firstHigh = emit("first high", {Message::NOT_PERSISTENT, 8});
  const auto secondHigh = emit("second high", {Message::PERSISTENT, 8});
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const std::vector<const TextMessage*> original{&firstHigh, &secondHigh, &low};
  const auto snapshot = browser.enumerate();
  ASSERT_NO_FATAL_FAILURE(expectSnapshot(snapshot, original));

  row("REQ-71-5", "concurrent send and excluded claim/ack leave the full ordered snapshot with or without new");
  TextMessage newMessage;
  std::barrier start(3);
  std::jthread sending([&] {
    start.arrive_and_wait();
    newMessage = emit("new", {Message::PERSISTENT, 4});
  });
  std::jthread consuming([&] {
    start.arrive_and_wait();
    auto removed = text(excluded, 1'000'000);
    ASSERT_NE(removed, nullptr);
    EXPECT_EQ(removed->text(), "unmatched");
    excluded->acknowledgeOn(*removed);
    consumer->acknowledgeOn(*held);
  });
  start.arrive_and_wait();
  auto concurrent = browser.enumerate();
  sending.join();
  consuming.join();
  const std::vector<const TextMessage*> afterSend{&firstHigh, &secondHigh, &newMessage, &low};
  ASSERT_NO_FATAL_FAILURE(expectSnapshot(concurrent, concurrent.size() == afterSend.size() ? afterSend : original));
  EXPECT_NE(snapshot[0], concurrent[0]);
  EXPECT_NE(snapshot[1], concurrent[1]);
  EXPECT_NE(snapshot.back(), concurrent.back());
  Message::As<TextMessage>(concurrent.front())->setText("edited snapshot");

  row("REQ-71-5", "browser session recover leaves a single copy of every record with first-delivery flags");
  browse.recover();
  ASSERT_NO_FATAL_FAILURE(expectSnapshot(browser.enumerate(), afterSend));
  auto first = text(consumer);
  ASSERT_NE(first, nullptr);
  EXPECT_NE(first, snapshot.front());
  EXPECT_EQ(first->text(), "first high");
  EXPECT_EQ(first->jmsHeaders.jmsDeliveryCount(), 1);
  EXPECT_FALSE(first->jmsHeaders.redelivered);
  consumer->acknowledgeOn(*first);

  row("REQ-71-5", "actual receive recovers and acknowledges while the first snapshot keeps its contents and headers");
  auto pending = text(consumer);
  ASSERT_NE(pending, nullptr);
  EXPECT_EQ(pending->text(), "second high");
  EXPECT_EQ(pending->jmsHeaders.jmsDeliveryCount(), 1);
  EXPECT_FALSE(pending->jmsHeaders.redelivered);
  receive.recover();
  auto redelivered = text(consumer);
  ASSERT_NE(redelivered, nullptr);
  EXPECT_EQ(redelivered->text(), "second high");
  EXPECT_EQ(redelivered->jmsHeaders.jmsDeliveryCount(), 2);
  EXPECT_TRUE(redelivered->jmsHeaders.redelivered);
  consumer->acknowledgeOn(*redelivered);
  for (const std::string& expected : {newMessage.text(), low.text()}) {
    auto actual = text(consumer);
    ASSERT_NE(actual, nullptr);
    EXPECT_EQ(actual->text(), expected);
    EXPECT_EQ(actual->jmsHeaders.jmsDeliveryCount(), 1);
    EXPECT_FALSE(actual->jmsHeaders.redelivered);
    consumer->acknowledgeOn(*actual);
  }
  ASSERT_NO_FATAL_FAILURE(expectSnapshot(snapshot, original));

  row("REQ-71-5", "scheduled copy preserves JMSDeliveryTime and receive flags");
  auto actual = text(consumer, 1'000'000);
  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->text(), "delayed");
  EXPECT_EQ(actual->jmsHeaders.deliveryTime, delayed.jmsHeaders.deliveryTime);
  EXPECT_FALSE(actual->jmsHeaders.redelivered);
  consumer->acknowledgeOn(*actual);
  EXPECT_EQ(consumer->recv(0), nullptr);
  EXPECT_EQ(browser.enumerate().size(), 0u);

  row("REQ-71-5", "expiration leaves the original snapshot intact and excludes the entry from a fresh snapshot");
  const auto expiring = emit("expires after snapshot", {Message::PERSISTENT, 4, 300});
  const auto beforeExpiration = browser.enumerate();
  ASSERT_LT(Poco::Timestamp().epochMicroseconds() / 1000, expiring.jmsHeaders.expiration);
  ASSERT_NO_FATAL_FAILURE(expectSnapshot(beforeExpiration, {&expiring}));
  const auto expiration = std::chrono::system_clock::time_point{std::chrono::milliseconds(expiring.jmsHeaders.expiration)};
  std::this_thread::sleep_until(expiration + std::chrono::milliseconds(1));
  EXPECT_TRUE(browser.enumerate().empty());
  ASSERT_NO_FATAL_FAILURE(expectSnapshot(beforeExpiration, {&expiring}));
}

class ScheduledMessageProbe : public TextMessage {
 public:
  explicit ScheduledMessageProbe(std::shared_ptr<int> marker) : _marker(std::move(marker)) {
    setText("scheduled response");
  }

  Message::Ptr copy() const override { return std::make_shared<ScheduledMessageProbe>(*this); }

 private:
  std::shared_ptr<int> _marker;
};

TEST_F(DestinationContractTest, TemporaryReplyLifecycle) {
  Exchange exchange(path), unrelated(Poco::Path(path).append("other").makeDirectory());
  Connection replier(exchange), wrongBroker(unrelated);
  wrongBroker.start();
  replier.setClientID("same client ID");
  replier.start();
  auto& reply = replier.createSession();
  auto& wrong = wrongBroker.createSession();
  auto requests = reply.createDestination(destination::Queue, "requests");
  auto incoming = reply.createConsumer(requests);
  std::set<std::string> uris;

  const auto checkLifecycle = [&](bool queue, bool closeOwner) {
    const auto label = std::string(queue ? "TemporaryQueue" : "TemporaryTopic") +
        (closeOwner ? " owner close" : " explicit delete");
    SCOPED_TRACE(label);
    row("REQ-71-6", label.c_str());
    Connection owner(exchange);
    owner.setClientID("same client ID");
    owner.start();
    auto& ask = owner.createSession();
    auto outbound = ask.createProducer(requests);
    auto address = queue ? ask.createTemporaryQueue() : ask.createTemporaryTopic();
    auto consumer = ask.createConsumer(address);
    auto request = ask.createTextMessage("request");
    request.jmsHeaders.replyTo = address->uri();
    request.jmsHeaders.correlationId = "correlation";
    outbound->send(request);
    const auto received = text(incoming);
    ASSERT_NE(received, nullptr);
    const auto uri = received->jmsHeaders.replyTo;
    EXPECT_EQ(uri, address->uri());
    EXPECT_TRUE(uris.insert(uri).second);
    const auto name = uri.substr(uri.find("://") + 3);
    const auto resolved = reply.createDestination(address->type(), name);
    EXPECT_EQ(resolved, address);
    auto producer = reply.createProducer(resolved);
    auto response = reply.createTextMessage("response");
    response.jmsHeaders.correlationId = received->jmsHeaders.correlationId;
    producer->send(response, {Message::PERSISTENT});
    auto result = text(consumer);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->text(), "response");
    EXPECT_EQ(result->jmsHeaders.correlationId, "correlation");

    row("REQ-71-6", "foreign consume, browse and delete rejected, cross-broker send rejected");
    EXPECT_THROW(reply.createConsumer(resolved), IllegalStateException);
    if (queue) {
      EXPECT_THROW(reply.createQueueBrowser(resolved), IllegalStateException);
    }
    EXPECT_THROW(address->remove(replier), IllegalStateException);
    EXPECT_THROW(wrong.createProducer(resolved), std::invalid_argument);
    if (!queue) {
      EXPECT_THROW(ask.createSharedDurableConsumer(address, "durable"), std::invalid_argument);
    }

    std::weak_ptr<int> scheduled;
    int64_t deadline;
    {
      const auto marker = std::make_shared<int>(0);
      scheduled = marker;
      ScheduledMessageProbe probe(marker);
      producer->send(probe, {Message::PERSISTENT, 4, 0, 30'000});
      deadline = probe.jmsHeaders.deliveryTime;
    }
    row("REQ-71-6", "active consumer prevents delete while the scheduled copy remains pending");
    EXPECT_THROW(address->remove(owner), IllegalStateException);
    EXPECT_EQ(consumer->recv(0), nullptr);
    ASSERT_FALSE(scheduled.expired());

    std::optional<QueueBrowser> browser;
    if (queue) {
      browser.emplace(ask.createQueueBrowser(address));
    }
    if (closeOwner) {
      owner.close();
    } else {
      ask.deleteConsumer(consumer->id());
      address->remove(owner);
    }
    if (browser) {
      EXPECT_THROW(browser->enumerate(), IllegalStateException);
    }
    EXPECT_THROW(consumer->recv(0), IllegalStateException);
    auto& replacementSession = closeOwner ? reply : ask;
    auto replacement = queue ? replacementSession.createTemporaryQueue() : replacementSession.createTemporaryTopic();
    EXPECT_TRUE(uris.insert(replacement->uri()).second);
    replacement->remove(closeOwner ? replier : owner);
    EXPECT_THROW(reply.createDestination(address->type(), name), IllegalStateException);
    EXPECT_THROW(producer->send(response), IllegalStateException);
    reply.deleteProducer(producer->id());
    producer.reset();
    row("REQ-71-6", "scheduled copy released before its delivery deadline");
    EXPECT_LT(Poco::Timestamp().epochMicroseconds() / 1000, deadline);
    EXPECT_TRUE(scheduled.expired());
  };

  for (bool queue : {true, false}) {
    for (bool closeOwner : {false, true}) {
      checkLifecycle(queue, closeOwner);
    }
  }
}
}  // namespace
}  // namespace tiny_mq
