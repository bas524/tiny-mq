//
// Benchmark tests for tiny-mq: send/recv across all acknowledge modes
// and persistence settings.
//
// Run with: ./tiny_mq --gbench [--benchmark_filter=<regex>]
//
// Scenarios:
//   AUTO_ACKNOWLEDGE  + NOT_PERSISTENT  — pure in-memory queue
//   AUTO_ACKNOWLEDGE  + PERSISTENT      — auto-ack with storage I/O
//   CLIENT_ACKNOWLEDGE + PERSISTENT     — manual ack removes from storage
//   SESSION_TRANSACTED + NOT_PERSISTENT — transacted, in-memory commit
//   SESSION_TRANSACTED + PERSISTENT     — transacted, storage commit
//   SESSION_TRANSACTED batch (N=10/100/1000) NOT_PERSISTENT
//   SESSION_TRANSACTED batch (N=10/100/1000) PERSISTENT
//   Topic + AUTO_ACKNOWLEDGE + NOT_PERSISTENT — pub/sub single subscriber
//

#include "BenchmarkTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TextMessage.h"
#include "Selector.h"
#include <Poco/File.h>
#include <Poco/Logger.h>
#include <Poco/Channel.h>
#include <barrier>
#include <thread>

namespace {

// Silence all Poco logging during benchmarks to avoid I/O overhead.
class NullChannel : public Poco::Channel {
public:
    void log(const Poco::Message&) override {}
};

void silenceLogging() {
    static bool done = false;
    if (!done) {
        auto* ch = new NullChannel;
        Poco::Logger::root().setChannel(ch);
        Poco::Logger::root().setLevel(Poco::Message::PRIO_FATAL);
        done = true;
    }
}

constexpr const char* kBenchDir = "./bench-mq";
constexpr const char* kPayload =
    "benchmark-payload-0123456789abcdef"
    "benchmark-payload-0123456789abcdef"
    "benchmark-payload-0123456789ab";  // ~100 bytes

}  // namespace

void BenchmarkFixture::SetUp(::benchmark::State& /*state*/) {
    silenceLogging();
    Poco::File dir(kBenchDir);
    if (dir.exists()) {
        dir.remove(true);  // clean slate — no leftover persistent messages
    }
    exchange = std::make_unique<tiny_mq::Exchange>(kBenchDir);
}

void BenchmarkFixture::TearDown(::benchmark::State& /*state*/) {
    exchange.reset();
}

// ---------------------------------------------------------------------------
// 1. AUTO_ACKNOWLEDGE + NOT_PERSISTENT
//    Baseline: no storage I/O at all.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, AutoAck_NonPersistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    auto dest = session.createDestination(destination::Queue, "bm_auto_np");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::NOT_PERSISTENT);

    for (auto _ : state) {
        producer->send(msg);
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
    }
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// 2. AUTO_ACKNOWLEDGE + PERSISTENT
//    Each send appends to storage; recv reads & marks deleted.
// ---------------------------------------------------------------------------
BENCHMARK_DEFINE_F(BenchmarkFixture, AutoAck_Persistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    auto dest = session.createDestination(destination::Queue, "bm_auto_p");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::PERSISTENT);

    for (auto _ : state) {
        producer->send(msg);
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK_REGISTER_F(BenchmarkFixture, AutoAck_Persistent_RoundTrip)->MeasureProcessCPUTime();

// ---------------------------------------------------------------------------
// 3. CLIENT_ACKNOWLEDGE + PERSISTENT
//    Explicit acknowledgeOn() triggers storage delete.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, ClientAck_Persistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
    auto dest = session.createDestination(destination::Queue, "bm_client_p");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::PERSISTENT);

    for (auto _ : state) {
        producer->send(msg);
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
        consumer->acknowledgeOn(*received);
    }
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// 3b. CLIENT_ACKNOWLEDGE batch — NOT_PERSISTENT
//    send N -> recv N -> ack N. Exercises Consumer::_inFlight tracking
//    (spec 23, Session.recover()) across a whole batch instead of one
//    message at a time: ClientAck_Persistent_RoundTrip above interleaves
//    send/recv/ack per iteration (always n=1 in flight) and is storage-bound,
//    so it never surfaces the cost of removing an in-flight entry. This
//    benchmark isolates that cost — with batch=1000 it makes an O(n^2)
//    linear-scan-and-erase in acknowledgeOn() visible as a steep drop in
//    items/sec relative to batch=1/100.
//    Registered with batch sizes 1 / 100 / 1000.
// ---------------------------------------------------------------------------
BENCHMARK_DEFINE_F(BenchmarkFixture, ClientAck_Batch_NonPersistent)(benchmark::State& state) {
    using namespace tiny_mq;
    const int64_t batch = state.range(0);
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::CLIENT_ACKNOWLEDGE);
    auto dest = session.createDestination(destination::Queue, "bm_clientbatch_np");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);

    std::vector<TextMessage> msgs;
    msgs.reserve(static_cast<size_t>(batch));
    for (int64_t i = 0; i < batch; ++i) {
        msgs.push_back(session.createTextMessage(kPayload, Message::NOT_PERSISTENT));
    }

    for (auto _ : state) {
        for (int64_t i = 0; i < batch; ++i) {
            producer->send(msgs[static_cast<size_t>(i)]);
        }
        std::vector<Message::Ptr> received;
        received.reserve(static_cast<size_t>(batch));
        for (int64_t i = 0; i < batch; ++i) {
            received.push_back(consumer->recv());
        }
        for (auto& r : received) {
            consumer->acknowledgeOn(*r);
        }
        benchmark::DoNotOptimize(received);
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK_REGISTER_F(BenchmarkFixture, ClientAck_Batch_NonPersistent)
    ->Arg(1)->Arg(100)->Arg(1000);

// ---------------------------------------------------------------------------
// 4. SESSION_TRANSACTED + NOT_PERSISTENT
//    Commit buffers in memory only; no storage I/O.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, Transacted_NonPersistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    auto dest = session.createDestination(destination::Queue, "bm_tx_np");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::NOT_PERSISTENT);

    for (auto _ : state) {
        producer->send(msg);
        session.commit();
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
        session.commit();
    }
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// 5. SESSION_TRANSACTED + PERSISTENT
//    Commit flushes to storage; most expensive single-message path.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, Transacted_Persistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    auto dest = session.createDestination(destination::Queue, "bm_tx_p");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::PERSISTENT);

    for (auto _ : state) {
        producer->send(msg);
        session.commit();
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
        session.commit();
    }
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// 6. SESSION_TRANSACTED batch — NOT_PERSISTENT
//    Amortises commit overhead over N messages per transaction.
//    Registered with batch sizes 10 / 100 / 1000.
// ---------------------------------------------------------------------------
BENCHMARK_DEFINE_F(BenchmarkFixture, Transacted_Batch_NonPersistent)(benchmark::State& state) {
    using namespace tiny_mq;
    const int64_t batch = state.range(0);
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    auto dest = session.createDestination(destination::Queue, "bm_txbatch_np");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);

    std::vector<TextMessage> msgs;
    msgs.reserve(static_cast<size_t>(batch));
    for (int64_t i = 0; i < batch; ++i) {
        msgs.push_back(session.createTextMessage(kPayload, Message::NOT_PERSISTENT));
    }

    for (auto _ : state) {
        for (int64_t i = 0; i < batch; ++i) {
            producer->send(msgs[static_cast<size_t>(i)]);
        }
        session.commit();
        for (int64_t i = 0; i < batch; ++i) {
            auto r = consumer->recv();
            benchmark::DoNotOptimize(r);
        }
        session.commit();
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK_REGISTER_F(BenchmarkFixture, Transacted_Batch_NonPersistent)
    ->Arg(10)->Arg(100)->Arg(1000);

// ---------------------------------------------------------------------------
// 7. SESSION_TRANSACTED batch — PERSISTENT
//    Like above but each commit writes N records to storage.
// ---------------------------------------------------------------------------
BENCHMARK_DEFINE_F(BenchmarkFixture, Transacted_Batch_Persistent)(benchmark::State& state) {
    using namespace tiny_mq;
    const int64_t batch = state.range(0);
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    auto dest = session.createDestination(destination::Queue, "bm_txbatch_p");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);

    std::vector<TextMessage> msgs;
    msgs.reserve(static_cast<size_t>(batch));
    for (int64_t i = 0; i < batch; ++i) {
        msgs.push_back(session.createTextMessage(kPayload, Message::PERSISTENT));
    }

    for (auto _ : state) {
        for (int64_t i = 0; i < batch; ++i) {
            producer->send(msgs[static_cast<size_t>(i)]);
        }
        session.commit();
        for (int64_t i = 0; i < batch; ++i) {
            auto r = consumer->recv();
            benchmark::DoNotOptimize(r);
        }
        session.commit();
    }
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK_REGISTER_F(BenchmarkFixture, Transacted_Batch_Persistent)
    ->Arg(10)->Arg(100)->Arg(1000);

// ---------------------------------------------------------------------------
// 8. Topic + AUTO_ACKNOWLEDGE + NOT_PERSISTENT
//    One publisher, one subscriber — measures pub/sub fan-out overhead.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, Topic_AutoAck_NonPersistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    auto topic = session.createDestination(destination::Topic, "bm_topic_np");
    auto subscriber = session.createConsumer(topic);
    auto publisher = session.createProducer(topic);
    TextMessage msg = session.createTextMessage(kPayload, Message::NOT_PERSISTENT);

    for (auto _ : state) {
        publisher->send(msg);
        auto received = subscriber->recv();
        benchmark::DoNotOptimize(received);
    }
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// 9. Priority ordering — uniform priority (spec 45 open question).
//    All messages sent at default p=4.  This benchmark answers the open
//    question: does multi-band polling add unacceptable latency to the common
//    uniform-priority case?  A regression >~5% vs the baseline for
//    AutoAck_NonPersistent_RoundTrip is a blocker.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, Priority_Uniform_NonPersistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    auto dest = session.createDestination(destination::Queue, "bm_prio_uniform_np");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::NOT_PERSISTENT);
    // Default priority = 4 (uniform workload).
    producer->send(msg, SendOptions{Message::NOT_PERSISTENT, 4, 0, 0});
    auto dummy = consumer->recv();  // drain the one pre-sent message
    benchmark::DoNotOptimize(dummy);

    for (auto _ : state) {
        producer->send(msg, SendOptions{Message::NOT_PERSISTENT, 4, 0, 0});
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
    }
    state.SetItemsProcessed(state.iterations());
}

// ---------------------------------------------------------------------------
// 10. Priority ordering — mixed priorities (p=0 and p=9, interleaved sends).
//     Measures throughput when the priority path is exercised.
// ---------------------------------------------------------------------------
BENCHMARK_F(BenchmarkFixture, Priority_Mixed_NonPersistent_RoundTrip)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection session_conn(*exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
    auto dest = session.createDestination(destination::Queue, "bm_prio_mixed_np");
    auto producer = session.createProducer(dest);
    auto consumer = session.createConsumer(dest);
    TextMessage msg = session.createTextMessage(kPayload, Message::NOT_PERSISTENT);

    bool highPrio = true;
    for (auto _ : state) {
        int32_t prio = highPrio ? 9 : 0;
        highPrio = !highPrio;
        producer->send(msg, SendOptions{Message::NOT_PERSISTENT, prio, 0, 0});
        auto received = consumer->recv();
        benchmark::DoNotOptimize(received);
    }
    state.SetItemsProcessed(state.iterations());
}

static void MessageSelector(benchmark::State& state) {
    using namespace tiny_mq;
    TextMessage message({}, "selector payload");
    message.setIntProperty("rank", 123);
    message.setStringProperty("name", "James");
    auto selector = Selector::parse("rank > 100 AND name LIKE 'J_mes'");
    for (auto _ : state) {
        benchmark::DoNotOptimize(selector->matches(message));
    }
}
BENCHMARK(MessageSelector);

// REQ-71-7: both reader sessions claim in parallel. Report process CPU time
// across all threads and normalize a benchmark iteration to one delivery.
BENCHMARK_DEFINE_F(BenchmarkFixture, Destination_ConcurrentClaim)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection conn(*exchange);
    auto& send = conn.createSession();
    auto queue = send.createDestination(destination::Queue, "competing-claims");
    auto writer = send.createProducer(queue);
    auto left = conn.createSession().createConsumer(queue);
    auto right = conn.createSession().createConsumer(queue);
    auto message = send.createTextMessage(kPayload);
    constexpr int perReader = 128;
    std::barrier batch(3);
    std::atomic_bool finished{false};
    auto read = [&](const Consumer::Ptr& consumer) {
        for (;;) {
            batch.arrive_and_wait();
            if (finished.load(std::memory_order_relaxed)) return;
            for (int i = 0; i < perReader; ++i) {
                auto delivery = consumer->recv(-1);
                benchmark::DoNotOptimize(delivery);
            }
            batch.arrive_and_wait();
        }
    };
    std::jthread first(read, left);
    std::jthread second(read, right);
    for (auto _ : state) {
        batch.arrive_and_wait();
        for (int i = 0; i < perReader * 2; ++i) writer->send(message);
        batch.arrive_and_wait();
    }
    finished.store(true, std::memory_order_relaxed);
    batch.arrive_and_wait();
    state.SetItemsProcessed(state.iterations() * perReader * 2);
    state.counters["deliveries_per_iteration"] = perReader * 2;
}
BENCHMARK_REGISTER_F(BenchmarkFixture, Destination_ConcurrentClaim)->MeasureProcessCPUTime();

// REQ-71-7: measure the eligible fan-out on one, two and four subscriptions.
BENCHMARK_DEFINE_F(BenchmarkFixture, Destination_Fanout)(benchmark::State& state) {
    using namespace tiny_mq;
    Connection conn(*exchange);
    auto& session = conn.createSession();
    auto topic = session.createDestination(destination::Topic, "fanout");
    auto writer = session.createProducer(topic);
    std::vector<Consumer::Ptr> receivers;
    const auto count = state.range(0);
    for (int64_t i = 0; i < count; ++i) receivers.push_back(session.createConsumer(topic));
    auto message = session.createTextMessage(kPayload);
    for (auto _ : state) {
        writer->send(message);
        for (const auto& receiver : receivers) {
            auto delivery = receiver->recv(0);
            benchmark::DoNotOptimize(delivery);
        }
    }
    state.SetItemsProcessed(state.iterations() * count);
    state.counters["deliveries_per_iteration"] = static_cast<double>(count);
}
BENCHMARK_REGISTER_F(BenchmarkFixture, Destination_Fanout)->Arg(1)->Arg(2)->Arg(4);
