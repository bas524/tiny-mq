#include "BrokerServer.h"
#include "StompCodec.h"
#include "StompConnection.h"

#include <Poco/Exception.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Timespan.h>
#include <benchmark/benchmark.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iostream>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace {

using namespace tiny_mq;
using namespace std::chrono_literals;
using namespace std::string_view_literals;
using Poco::Net::StreamSocket;

constexpr auto kWait = 2s;
constexpr std::size_t kBudget = 8192;
constexpr WireTime kAccept{1s};
constexpr auto kConnected = "CONNECTED\nversion:1.2\nheart-beat:0,0\n\n\0"sv;
constexpr auto kConnect = "CONNECT\naccept-version:1.2\nhost:localhost\n\n\0"sv;
constexpr auto kDisconnect = "DISCONNECT\nreceipt:s\n\n\0"sv;
constexpr auto kReceipt = "RECEIPT\nreceipt-id:s\n\n\0"sv;

std::span<const std::byte> bytes(std::string_view data) {
    return std::as_bytes(std::span(data.data(), data.size()));
}

std::string octets(std::span<const std::byte> data) {
    if (data.empty()) {
        return {};
    }
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

std::vector<std::string> blocks(const WireResult& result) {
    std::vector<std::string> out;
    for (const auto& block : result.output) {
        out.push_back(octets(block));
    }
    return out;
}

std::string frame(std::string_view command, std::string_view headers = {},
                  std::string_view body = {}, std::string_view eol = "\n") {
    std::string result(command);
    result.append(eol).append(headers).append(eol).append(body).push_back('\0');
    return result;
}

std::string handshake(std::string_view headers = {}, std::string_view command = "CONNECT") {
    std::string all = "accept-version:1.2\nhost:localhost\n";
    return frame(command, all.append(headers));
}

std::string errorFrame(std::string_view code) {
    std::string headers = "message:";
    headers.append(code).append("\n");
    if (code == "version") {
        headers.append("version:1.2\n");
    }
    return frame("ERROR", headers);
}

std::string receipt(std::string_view id) {
    std::string headers = "receipt-id:";
    headers.append(id).append("\n");
    return frame("RECEIPT", headers);
}

WireLimits quietLimits() {
    WireLimits limits;
    limits.heartbeat_send = 0ms;
    limits.heartbeat_receive = 0ms;
    return limits;
}

std::optional<StreamSocket> connectPeer(std::uint16_t port) {
    try {
        StreamSocket peer;
        peer.connect(Poco::Net::SocketAddress("127.0.0.1", port), Poco::Timespan(2, 0));
        peer.setNoDelay(true);
        peer.setReceiveTimeout(Poco::Timespan(2, 0));
        peer.setSendTimeout(Poco::Timespan(2, 0));
        return peer;
    } catch (const Poco::Exception&) {
        return std::nullopt;
    }
}

bool writePeer(StreamSocket& peer, std::string_view data) {
    try {
        while (!data.empty()) {
            const int sent = peer.sendBytes(data.data(), static_cast<int>(data.size()));
            if (sent <= 0) {
                return false;
            }
            data.remove_prefix(static_cast<std::size_t>(sent));
        }
        return true;
    } catch (const Poco::Exception&) {
        return false;
    }
}

int readPeer(StreamSocket& peer, char* output, int capacity, WireTime deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= 0us) {
        return -1;
    }
    try {
        if (!peer.poll(Poco::Timespan(remaining.count()), Poco::Net::Socket::SELECT_READ)) {
            return -1;
        }
        return peer.receiveBytes(output, capacity);
    } catch (const Poco::Net::ConnectionResetException&) {
        return 0;
    } catch (const Poco::Net::ConnectionAbortedException&) {
        return 0;
    } catch (const Poco::Exception&) {
        return -1;
    }
}

struct PeerRead {
    std::string data;
    bool closed = false;
};

PeerRead readClosed(StreamSocket& peer) {
    PeerRead result;
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    std::array<char, 256> buffer{};
    while (result.data.size() < kBudget) {
        const int size = readPeer(peer, buffer.data(), static_cast<int>(buffer.size()), deadline);
        if (size <= 0) {
            result.closed = size == 0;
            return result;
        }
        result.data.append(buffer.data(), static_cast<std::size_t>(size));
    }
    return result;
}

std::optional<std::string> readExactly(StreamSocket& peer, std::size_t size) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    std::string result(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const int count = readPeer(peer, result.data() + offset,
                                   static_cast<int>(size - offset), deadline);
        if (count <= 0) {
            return std::nullopt;
        }
        offset += static_cast<std::size_t>(count);
    }
    return result;
}

std::string readFrame(StreamSocket& peer) {
    const auto deadline = std::chrono::steady_clock::now() + kWait;
    std::string result;
    while (result.size() < kBudget) {
        char value{};
        if (readPeer(peer, &value, 1, deadline) != 1) {
            break;
        }
        result.push_back(value);
        if (value == '\0') {
            break;
        }
    }
    return result;
}

void expectConnected(std::string_view data, std::string_view heartbeat = "0,0",
                     bool framing = false) {
    EXPECT_TRUE(data.starts_with("CONNECTED\n")) << data;
    EXPECT_TRUE(data.ends_with("\n\n\0"sv)) << data;
    EXPECT_NE(data.find("\nversion:1.2\n"), std::string_view::npos) << data;
    std::string line = "\nheart-beat:";
    line.append(heartbeat).append("\n");
    EXPECT_NE(data.find(line), std::string_view::npos) << data;
    if (framing) {
        EXPECT_NE(data.find("\nx-tinymq-capabilities:framing-v1\n"), std::string_view::npos);
    } else {
        EXPECT_EQ(data.find("\nx-tinymq-capabilities:"), std::string_view::npos);
    }
    EXPECT_EQ(data.find("\nx-tinymq-required-capabilities:"), std::string_view::npos);
}

void expectClosed(StreamSocket& peer, std::string_view output) {
    const auto result = readClosed(peer);
    EXPECT_EQ(result.data, output);
    EXPECT_TRUE(result.closed) << "peer did not close within deadline";
}

void finishNeighbor(StreamSocket& peer) {
    ASSERT_TRUE(writePeer(peer, kDisconnect));
    expectClosed(peer, kReceipt);
}

void expectFailureWithNeighbor(std::uint16_t port, std::string_view input,
                               std::string_view error, bool afterHandshake = false) {
    auto neighbor = connectPeer(port);
    ASSERT_TRUE(neighbor.has_value());
    ASSERT_TRUE(writePeer(*neighbor, kConnect));
    ASSERT_NO_FATAL_FAILURE(expectConnected(readFrame(*neighbor)));
    auto peer = connectPeer(port);
    ASSERT_TRUE(peer.has_value());
    if (afterHandshake) {
        ASSERT_TRUE(writePeer(*peer, kConnect));
        ASSERT_NO_FATAL_FAILURE(expectConnected(readFrame(*peer)));
    }
    ASSERT_TRUE(writePeer(*peer, input));
    expectClosed(*peer, errorFrame(error));
    finishNeighbor(*neighbor);
}

class ManualClock {
public:
    WireTime now() const {
        const std::lock_guard lock(mutex_);
        ++samples_;
        sampled_.notify_all();
        return WireTime{ticks_};
    }

    void advance(std::chrono::milliseconds by, BrokerServer& server) {
        {
            const std::lock_guard lock(mutex_);
            ticks_ += by;
        }
        server.notifyClockChanged();
    }

    bool observeClockAtCurrentTime(BrokerServer& server) {
        std::unique_lock lock(mutex_);
        const auto before = samples_;
        lock.unlock();
        server.notifyClockChanged();
        lock.lock();
        return sampled_.wait_for(lock, kWait, [&] { return samples_ > before; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable sampled_;
    mutable std::size_t samples_ = 0;
    std::chrono::milliseconds ticks_{1000};
};

class ScriptedWriter {
public:
    void allow(std::size_t amount) {
        const std::lock_guard lock(mutex_);
        allowance_ = amount;
    }

    void fail(WireWriteResult value, bool throwInstead = false) {
        const std::lock_guard lock(mutex_);
        override_ = value;
        throw_ = throwInstead;
    }

    void clearFailure() {
        const std::lock_guard lock(mutex_);
        override_.reset();
        throw_ = false;
    }

    void capChunk(std::size_t limit) {
        const std::lock_guard lock(mutex_);
        writeCap_ = limit;
    }

    void watchInboundOnFirstWrite(std::size_t size) {
        const std::lock_guard lock(mutex_);
        pendingInbound_ = size;
    }

    bool waitFirstWrite() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, kWait, [&] { return started_; });
    }

    bool waitInboundConsumed() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, kWait, [&] { return inboundConsumed_; });
    }

    WireWriteResult write(StreamSocket& socket, std::span<const std::byte> data) {
        std::unique_lock lock(mutex_);
        descriptor_ = static_cast<std::uint64_t>(socket.impl()->sockfd());
        if (throw_) {
            throw std::runtime_error("scripted write exception");
        }
        if (override_.has_value()) {
            return *override_;
        }
        if (!started_) {
            started_ = true;
            changed_.notify_all();
            if (pendingInbound_ != 0) {
                const auto expected = pendingInbound_;
                lock.unlock();
                const bool arrived = socket.poll(Poco::Timespan(2, 0), Poco::Net::Socket::SELECT_READ) &&
                    socket.available() >= static_cast<int>(expected);
                lock.lock();
                inboundReady_ = arrived;
            }
        } else if (inboundReady_ && !inboundConsumed_ && socket.available() == 0) {
            inboundConsumed_ = true;
            changed_.notify_all();
        }
        WireWriteResult result{WireWriteStatus::would_block, 0};
        if (allowance_ != 0) {
            const auto n = static_cast<int>(std::min({allowance_, writeCap_, data.size()}));
            const int sent = socket.sendBytes(data.data(), n);
            if (sent > 0) {
                result = {WireWriteStatus::progress, static_cast<std::size_t>(sent)};
                if (allowance_ != std::numeric_limits<std::size_t>::max()) {
                    allowance_ -= result.bytes;
                }
            }
        }
        if (result.status == WireWriteStatus::would_block) {
            ++blocked_;
            lastBlocked_ = octets(data);
        }
        changed_.notify_all();
        return result;
    }

    bool waitBlocked(std::string_view suffix) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, kWait, [&] {
            return blocked_ > 0 && lastBlocked_ == suffix;
        });
    }

    std::optional<std::uint64_t> descriptor() const {
        const std::lock_guard lock(mutex_);
        return descriptor_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t allowance_ = 0;
    std::size_t writeCap_ = std::numeric_limits<std::size_t>::max();
    std::size_t blocked_ = 0;
    std::size_t pendingInbound_ = 0;
    std::string lastBlocked_;
    std::optional<std::uint64_t> descriptor_;
    std::optional<WireWriteResult> override_;
    bool started_ = false;
    bool inboundReady_ = false;
    bool inboundConsumed_ = false;
    bool throw_ = false;
};

struct Protocol {
    explicit Protocol(WireLimits limits = quietLimits()) : connection(limits, kAccept) {}

    WireResult handle(WireEvent event, WireTime now = kAccept, std::size_t budget = kBudget) {
        return connection.handle({now, budget, event});
    }

    WireResult input(std::string_view data, WireTime now = kAccept,
                     std::size_t budget = kBudget) {
        return handle(WireBytes{bytes(data)}, now, budget);
    }

    void establish(WireTime now = kAccept) {
        const auto result = input(kConnect, now);
        ASSERT_EQ(blocks(result), std::vector<std::string>{std::string(kConnected)});
        ASSERT_EQ(result.close, WireClose::keep);
        const auto written = handle(WireWritten{kConnected.size()}, now);
        EXPECT_EQ(written.close, WireClose::keep);
    }

    StompConnection connection;
};

void expectResult(const WireResult& result, std::initializer_list<std::string> output,
                  WireClose close, std::optional<WireTime> deadline) {
    EXPECT_EQ(blocks(result), std::vector<std::string>(output));
    EXPECT_EQ(result.close, close);
    EXPECT_EQ(result.deadline, deadline);
}

void checkDecoded(const StompFrame& actual, const StompFrame& expected) {
    EXPECT_EQ(actual.command, expected.command);
    EXPECT_EQ(actual.headers, expected.headers);
    EXPECT_EQ(actual.body, expected.body);
}

std::vector<StompFrame> decodeChunks(const std::vector<std::string_view>& chunks,
                                    WireLimits limits = WireLimits{}) {
    StompCodec codec(limits);
    std::vector<StompFrame> result;
    for (const auto chunk : chunks) {
        codec.feed(bytes(chunk), [&](StompFrame&& item) { result.push_back(std::move(item)); });
    }
    return result;
}

void expectDecoderError(std::string_view input, std::string_view error, WireLimits limits = {}) {
    StompCodec codec(limits);
    std::size_t decoded = 0;
    std::string diagnostic;
    try {
        codec.feed(bytes(input), [&](StompFrame&&) { ++decoded; });
    } catch (const std::runtime_error& exception) {
        diagnostic = exception.what();
    }
    EXPECT_EQ(diagnostic, error);
    EXPECT_EQ(decoded, 0u);
}

std::vector<std::byte> owningBytes(std::string_view data) {
    const auto view = bytes(data);
    return {view.begin(), view.end()};
}

void checkDecoder() {
    const auto binary = "\xff\0a"sv;
    const std::string stream = frame("MESSAGE", "x:é\\r\\n\\c\\\\\r\nx:ignored\r\n"
        " space : padded \r\ncontent-length:3\r\n", binary, "\r\n") +
        "\n\r\n" + frame("CONNECTED", "literal:\\q\n");
    const std::vector<StompFrame> expected{
        {"MESSAGE", {{"x", "é\r\n:\\"}, {" space ", " padded "}, {"content-length", "3"}},
         owningBytes(binary)},
        {"CONNECTED", {{"literal", "\\q"}}, {}},
    };
    auto verify = [&](const std::vector<std::string_view>& chunks) {
        const auto decoded = decodeChunks(chunks);
        ASSERT_EQ(decoded.size(), expected.size());
        for (std::size_t index = 0; index < decoded.size(); ++index) {
            checkDecoded(decoded[index], expected[index]);
        }
    };
    const std::string_view data(stream);
    ASSERT_NO_FATAL_FAILURE(verify({data}));
    for (std::size_t cut = 0; cut <= data.size(); ++cut) {
        SCOPED_TRACE(cut);
        ASSERT_NO_FATAL_FAILURE(verify({data.substr(0, cut), data.substr(cut)}));
    }
    std::vector<std::string_view> singleOctets;
    for (std::size_t index = 0; index < data.size(); ++index) {
        singleOctets.push_back(data.substr(index, 1));
    }
    ASSERT_NO_FATAL_FAILURE(verify(singleOctets));

    const auto nulHeader = decodeChunks({frame("SEND", "a\0b:c\0d\n"sv)});
    ASSERT_EQ(nulHeader.size(), 1u);
    ASSERT_EQ(nulHeader.front().headers.size(), 1u);
    EXPECT_EQ(nulHeader.front().headers.front().first, "a\0b"sv);
    EXPECT_EQ(nulHeader.front().headers.front().second, "c\0d"sv);

    const auto escape = decodeChunks({handshake("passcode:p\\cq\n"),
                                      handshake("passcode:p\\cq\n", "STOMP")});
    ASSERT_EQ(escape.size(), 2u);
    ASSERT_EQ(escape[0].headers.size(), 3u);
    ASSERT_EQ(escape[1].headers.size(), 3u);
    EXPECT_EQ(escape[0].headers.back().second, "p\\cq");
    EXPECT_EQ(escape[1].headers.back().second, "p:q");

    for (const auto command : {"SEND", "MESSAGE", "ERROR"}) {
        const auto payload = "\xff\0abcde"sv;
        const auto decoded = decodeChunks({frame(command, "content-length:007\n", payload)});
        ASSERT_EQ(decoded.size(), 1u);
        EXPECT_EQ(decoded.front().body, owningBytes(payload));
    }
    {
        StompCodec codec(WireLimits{});
        int count = 0;
        auto sink = [&](StompFrame&&) { ++count; };
        codec.feed(bytes("SEND\ncontent-length:3\n\nab"sv), sink);
        codec.eof();
        EXPECT_EQ(count, 0);
    }
}

std::vector<std::string> invalidFrames() {
    std::vector<std::string> result{
        frame("STOMP", "x:\\q\n"), frame("STOMP", "x:x\\\n"),
        frame("STOMP", "x:\xc0\xaf\n"), frame("STOMP", "x:\xed\xa0\x80\n"),
        frame("STOMP", "x:\xc3\n"), frame("STOMP", "x:one\rtwo\n"),
        frame("STOMP", "no-delimiter\n"), frame("STO\0MP"sv),
        frame("SEND", "content-length:\n"), frame("SEND", "content-length:+1\n", "x"),
        frame("SEND", "content-length: 1\n", "x"), frame("SEND", "content-length:0\n", "x"),
    };
    for (const auto command : {"CONNECT", "STOMP", "DISCONNECT", "ACK", "SUBSCRIBE", "UNKNOWN"}) {
        result.push_back(frame(command, {}, "x"));
        result.push_back(frame(command, "content-length:1\n", "x"));
    }
    return result;
}

void checkConfiguredLimits() {
    const WireLimits defaults;
    EXPECT_EQ(defaults.frame_bytes, 1048576u);
    EXPECT_EQ(defaults.header_bytes, 8192u);
    EXPECT_EQ(defaults.header_count, 64u);
    EXPECT_EQ(defaults.handshake, 5000ms);
    EXPECT_EQ(defaults.output_bytes, 1056768u);
    EXPECT_EQ(defaults.flush, 1000ms);
    EXPECT_EQ(defaults.heartbeat_send, 10000ms);
    EXPECT_EQ(defaults.heartbeat_receive, 10000ms);

    auto invalid = [&](auto member, auto value) {
        WireLimits limits;
        limits.*member = value;
        EXPECT_THROW({ BrokerServer server(limits); }, Poco::InvalidArgumentException);
        EXPECT_THROW({ BrokerServer server(limits, BrokerServer::productionDependencies()); },
                     Poco::InvalidArgumentException);
    };
    for (auto member : {&WireLimits::frame_bytes, &WireLimits::header_bytes,
                        &WireLimits::header_count, &WireLimits::output_bytes}) {
        invalid(member, std::size_t{0});
    }
    for (auto member : {&WireLimits::handshake, &WireLimits::flush}) {
        invalid(member, 0ms);
        invalid(member, -1ms);
    }
    invalid(&WireLimits::heartbeat_send, -1ms);
    invalid(&WireLimits::heartbeat_receive, -1ms);
    using WideTicks = std::chrono::duration<long double, std::chrono::steady_clock::period>;
    const auto maxMs = std::chrono::milliseconds::max();
    const auto maxSteady = WideTicks(std::chrono::steady_clock::duration::max()).count();
    // Use a value known not to fit before attempting any integral conversion.
    const auto ticks = WideTicks(maxMs).count();
    if (ticks > maxSteady) {
        for (auto member : {&WireLimits::handshake, &WireLimits::flush, &WireLimits::heartbeat_send}) {
            invalid(member, maxMs);
        }
    }
    if (2 * ticks > maxSteady) {
        invalid(&WireLimits::heartbeat_receive, maxMs);
    }
    EXPECT_NO_THROW({ BrokerServer server(quietLimits()); });
    EXPECT_NO_THROW({
        BrokerServer server(quietLimits(), BrokerServer::productionDependencies());
    });
}

void checkReceiptBudgets() {
    const auto end = kAccept + quietLimits().handshake;
    const std::string connected(kConnected);
    for (auto command : {"CONNECT", "STOMP"}) {
        for (std::size_t budget : {62u, 61u}) {
            SCOPED_TRACE(command);
            SCOPED_TRACE(budget);
            Protocol protocol;
            const auto result = protocol.input(handshake("receipt:r\n", command), kAccept, budget);
            if (budget == 62) {
                expectResult(result, {connected, receipt("r")}, WireClose::keep, end);
            } else {
                expectResult(result, {}, WireClose::immediate, std::nullopt);
            }
        }
    }
    for (std::size_t budget : {85u, 84u}) {
        Protocol protocol;
        const auto input = handshake("receipt:r\n") + std::string(kDisconnect);
        const auto result = protocol.input(input, kAccept, budget);
        if (budget == 85) {
            expectResult(result, {connected, receipt("r"), std::string(kReceipt)},
                         WireClose::after_flush, end);
        } else {
            expectResult(result, {}, WireClose::immediate, std::nullopt);
        }
    }
    Protocol receiptOnlyInHandshake;
    expectResult(receiptOnlyInHandshake.input(handshake("receipt:r\n") + frame("DISCONNECT")),
                 {connected, receipt("r")}, WireClose::after_flush, end);

    const auto escaping = "X\\r\\n\\c\\\\é"sv;
    const auto nul = "a\0b"sv;
    for (const auto id : {escaping, nul}) {
        const auto expected = receipt(id);
        ASSERT_EQ(expected.size(), id == escaping ? 33u : 25u);
        for (const auto budget : {expected.size(), expected.size() - 1}) {
            Protocol protocol;
            ASSERT_NO_FATAL_FAILURE(protocol.establish());
            const auto input = frame("DISCONNECT", std::string("receipt:").append(id).append("\n"));
            const auto result = protocol.input(input, kAccept, budget);
            if (budget == expected.size()) {
                expectResult(result, {expected}, WireClose::after_flush, std::nullopt);
            } else {
                expectResult(result, {}, WireClose::immediate, std::nullopt);
            }
        }
    }
    for (const std::string& finishing : {std::string(kDisconnect), frame("DISCONNECT", "bad-header\n")}) {
        for (const auto tail : {kConnect, "\0not-a-frame\n"sv}) {
            Protocol baseline;
            Protocol coalesced;
            ASSERT_NO_FATAL_FAILURE(baseline.establish());
            ASSERT_NO_FATAL_FAILURE(coalesced.establish());
            const auto expected = baseline.input(finishing);
            const auto actual = coalesced.input(finishing + std::string(tail));
            const bool disconnect = finishing == kDisconnect;
            expectResult(expected, {disconnect ? std::string(kReceipt) : errorFrame("syntax")},
                         WireClose::after_flush, std::nullopt);
            EXPECT_EQ(blocks(actual), blocks(expected));
            EXPECT_EQ(actual.close, expected.close);
            EXPECT_EQ(actual.deadline, expected.deadline);
        }
    }
}

void checkHandshakeErrors(std::uint16_t port) {
    using Entry = std::pair<std::string, std::string_view>;
    const std::vector<Entry> cases{
        {frame("CONNECT", "host:localhost\n"), "version"},
        {frame("CONNECT", "accept-version:1.0,1.1\nhost:localhost\nreceipt:inbound-only\n"), "version"},
        {frame("CONNECT", "accept-version:\nhost:localhost\n"), "syntax"},
        {frame("CONNECT", "accept-version:1.1,,1.2\nhost:localhost\n"), "syntax"},
        {frame("CONNECT", "accept-version:1.2,\nhost:localhost\n"), "syntax"},
        {frame("CONNECT", "accept-version: 1.2\nhost:localhost\n"), "syntax"},
        {frame("CONNECT", "accept-version:1.2\n"), "syntax"},
        {frame("CONNECT", "accept-version:1.2\nhost:\n"), "syntax"},
        {handshake("heart-beat:20, 30\n"), "syntax"},
        {handshake("heart-beat:99999999999999999999999999,0\n"), "syntax"},
        {handshake("x-tinymq-capabilities:\n"), "capability"},
        {handshake("x-tinymq-required-capabilities:\n"), "capability"},
        {handshake("x-tinymq-capabilities:framing-v1,framing-v1\n"), "capability"},
        {handshake("x-tinymq-capabilities:framing-v1, optional\n"), "capability"},
        {handshake("x-tinymq-capabilities:framing-v1,\n"), "capability"},
        {handshake("x-tinymq-capabilities:unknown\nx-tinymq-required-capabilities:unknown\n"), "capability"},
        {handshake("x-tinymq-capabilities:unknown\nx-tinymq-required-capabilities:framing-v1\n"), "capability"},
        {handshake("x-unrecognized:\\q\n", "STOMP"), "syntax"},
        {frame("SEND"), "unsupported-command"},
    };
    for (const auto& [input, code] : cases) {
        SCOPED_TRACE(input);
        ASSERT_NO_FATAL_FAILURE(expectFailureWithNeighbor(port, input, code));
    }
    for (const auto command : {"SEND", "SUBSCRIBE", "UNSUBSCRIBE", "ACK", "NACK", "BEGIN", "COMMIT",
                               "ABORT", "CONNECT", "STOMP", "MESSAGE", "RECEIPT", "ERROR", "UNKNOWN"}) {
        SCOPED_TRACE(command);
        ASSERT_NO_FATAL_FAILURE(expectFailureWithNeighbor(port, frame(command),
                                                           "unsupported-command", true));
    }
    ASSERT_NO_FATAL_FAILURE(expectFailureWithNeighbor(port, frame("ACK", "content-length:0\n"),
                                                       "unsupported-command", true));
    for (const auto& input : invalidFrames()) {
        SCOPED_TRACE(input);
        expectDecoderError(input, "syntax");
        ASSERT_NO_FATAL_FAILURE(expectFailureWithNeighbor(port, input, "syntax", true));
    }
}

void checkPositivePeers(std::uint16_t port) {
    for (const auto command : {"CONNECT", "STOMP"}) {
        SCOPED_TRACE(command);
        const bool crlf = std::string_view(command) == "STOMP";
        const auto headers = crlf ? "accept-version:1.2,2.0\r\nhost:localhost\r\n"
                                  : "accept-version:1.1,1.2\nhost:localhost\n";
        const auto input = frame(command, headers, {}, crlf ? "\r\n" : "\n");
        auto peer = connectPeer(port);
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, input));
        const auto normal = readFrame(*peer);
        ASSERT_TRUE(normal.starts_with("CONNECTED\n")) << normal;
        expectConnected(normal);
        finishNeighbor(*peer);

        peer = connectPeer(port);
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, handshake("login:synthetic-user\npasscode:p\\cq\nx-unknown:hidden\n",
                                              command)));
        EXPECT_EQ(readFrame(*peer), normal);
        finishNeighbor(*peer);

        for (const auto handshakeId : {"r", ""}) {
            peer = connectPeer(port);
            ASSERT_TRUE(peer.has_value());
            ASSERT_TRUE(writePeer(*peer, handshake(std::string("receipt:").append(handshakeId).append("\n"),
                                                  command)));
            EXPECT_EQ(readFrame(*peer), normal);
            EXPECT_EQ(readFrame(*peer), receipt(handshakeId));
            ASSERT_TRUE(writePeer(*peer, frame("DISCONNECT", "receipt:\n")));
            expectClosed(*peer, receipt(""));
        }
    }
    for (bool required : {false, true}) {
        auto peer = connectPeer(port);
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, handshake(required
            ? "x-tinymq-capabilities:framing-v1,optional\nx-tinymq-required-capabilities:framing-v1\n"
            : "x-tinymq-capabilities:optional\n")));
        expectConnected(readFrame(*peer), "0,0", required);
        finishNeighbor(*peer);
    }
    auto peer = connectPeer(port);
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(writePeer(*peer, std::string(kConnect) + std::string(kDisconnect)));
    expectClosed(*peer, std::string(kConnected) + std::string(kReceipt));
    for (const auto id : {"X\\r\\n\\c\\\\é"sv, "a\0b"sv}) {
        peer = connectPeer(port);
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, kConnect));
        EXPECT_EQ(readFrame(*peer), kConnected);
        ASSERT_TRUE(writePeer(*peer, frame("DISCONNECT", std::string("receipt:").append(id).append("\n"))));
        expectClosed(*peer, receipt(id));
    }

    peer = connectPeer(port);
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(writePeer(*peer, "CONNECT\naccept-version:1.2\nhost:localhost\n\n"sv));
    peer->shutdownSend();
    expectClosed(*peer, {});
}

void checkFrameLimits() {
    WireLimits limits = quietLimits();
    limits.frame_bytes = 64;
    const auto atLimit = frame("SEND", "content-length:39\n", std::string(39, 'x'));
    ASSERT_EQ(atLimit.size(), 64u);
    const auto decoded = decodeChunks({atLimit}, limits);
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ(decoded.front().body, owningBytes(std::string(39, 'x')));

    const auto legalAtLimit = handshake(std::string("x:").append(64 - kConnect.size() - 3, 'a').append("\n"));
    BrokerServer byteLimited(limits);
    const auto bytePort = byteLimited.startLoopback();
    auto peer = connectPeer(bytePort);
    ASSERT_TRUE(peer.has_value());
    ASSERT_EQ(legalAtLimit.size(), 64u);
    ASSERT_TRUE(writePeer(*peer, legalAtLimit));
    EXPECT_EQ(readFrame(*peer), kConnected);
    finishNeighbor(*peer);
    const auto aboveLimit = handshake(std::string("x:").append(65 - kConnect.size() - 3, 'a').append("\n"));
    expectFailureWithNeighbor(bytePort, aboveLimit, "limit");
    expectFailureWithNeighbor(bytePort, "SEND\ncontent-length:40\n\n", "limit", true);
    expectFailureWithNeighbor(bytePort, frame("SEND", "content-length:1\n", "ab"), "syntax", true);
    byteLimited.stop();

    limits.frame_bytes = 256;
    limits.header_bytes = 18;  // accept-version:1.2 occupies 18 octets.
    limits.header_count = 3;
    BrokerServer headerLimited(limits);
    const auto headerPort = headerLimited.startLoopback();
    peer = connectPeer(headerPort);
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(writePeer(*peer, handshake("x:" + std::string(16, 'a') + "\n")));
    EXPECT_EQ(readFrame(*peer), kConnected);
    finishNeighbor(*peer);
    expectFailureWithNeighbor(headerPort, handshake("x:" + std::string(17, 'a') + "\n"), "limit");
    expectFailureWithNeighbor(headerPort, handshake("x:a\nx:b\n"), "limit");
    headerLimited.stop();

    limits.header_bytes = 128;
    const auto overflow = std::to_string(std::numeric_limits<std::size_t>::max()) + "0";
    BrokerServer largeLength(limits);
    const auto port = largeLength.startLoopback();
    expectFailureWithNeighbor(port, "SEND\ncontent-length:" + overflow + "\n\n", "limit", true);
    expectFailureWithNeighbor(port, "SEND\ncontent-length:" + overflow + "X\n\n", "syntax", true);
}

void checkDirectHeartbeat() {
    WireLimits limits;
    limits.heartbeat_send = 10ms;
    limits.heartbeat_receive = 40ms;
    limits.handshake = 200ms;
    const auto end = kAccept + limits.handshake;
    const auto start = kAccept + 15ms;
    const auto connected = frame("CONNECTED", "version:1.2\nheart-beat:10,40\n");
    const auto input = handshake("heart-beat:20,30\n");
    for (const auto at : {end - 1ms, end, end + 1ms}) {
        Protocol protocol(limits);
        expectResult(protocol.handle(WireTimer{}), {}, WireClose::keep, end);
        expectResult(protocol.input(input, kAccept + 5ms), {connected}, WireClose::keep, end);
        const auto result = protocol.handle(WireWritten{connected.size()}, at);
        if (at < end) {
            expectResult(result, {}, WireClose::keep, at + 30ms);
        } else {
            expectResult(result, {}, WireClose::immediate, std::nullopt);
        }
    }
    Protocol protocol(limits);
    expectResult(protocol.input(input, kAccept + 5ms), {connected}, WireClose::keep, end);
    expectResult(protocol.handle(WireWritten{connected.size() - 1}, start - 1ms),
                 {}, WireClose::keep, end);
    expectResult(protocol.handle(WireWritten{1}, start), {}, WireClose::keep, start + 30ms);
    expectResult(protocol.handle(WireTimer{}, start + 29ms), {}, WireClose::keep, start + 30ms);
    expectResult(protocol.handle(WireTimer{}, start + 30ms), {"\n"}, WireClose::keep, start + 80ms);
    expectResult(protocol.handle(WireTimer{}, start + 40ms), {}, WireClose::keep, start + 80ms);
    expectResult(protocol.handle(WireWritten{1}, start + 50ms), {}, WireClose::keep, start + 80ms);
    expectResult(protocol.input("\r\n"sv, start + 80ms), {"\n"}, WireClose::keep, start + 160ms);
    expectResult(protocol.handle(WireTimer{}, start + 160ms),
                 {errorFrame("heartbeat-timeout")}, WireClose::after_flush, std::nullopt);

    const auto exactTie = handshake("heart-beat:20,80\n");
    Protocol tie(limits);
    expectResult(tie.input(exactTie), {connected}, WireClose::keep, end);
    expectResult(tie.handle(WireWritten{connected.size()}, start), {}, WireClose::keep, start + 80ms);
    expectResult(tie.handle(WireTimer{}, start + 80ms),
                 {errorFrame("heartbeat-timeout")}, WireClose::after_flush, std::nullopt);
    for (const auto header : {""sv, "heart-beat:0,0\n"sv, "heart-beat:0,30\n"sv,
                              "heart-beat:20,0\n"sv}) {
        Protocol disabled(limits);
        expectResult(disabled.input(handshake(header)), {connected}, WireClose::keep, end);
        std::optional<WireTime> deadline;
        if (header == "heart-beat:0,30\n") {
            deadline = start + 30ms;
        } else if (header == "heart-beat:20,0\n") {
            deadline = start + 80ms;
        }
        expectResult(disabled.handle(WireWritten{connected.size()}, start), {}, WireClose::keep, deadline);
    }
    Protocol unfinished(limits);
    expectResult(unfinished.input("CONNECT\n"), {}, WireClose::keep, end);
    expectResult(unfinished.input("accept-version:1.2\nhost:localhost\n\n\0"sv, end),
                 {errorFrame("handshake-timeout")}, WireClose::after_flush, std::nullopt);
    Protocol pending(limits);
    expectResult(pending.input(input), {connected}, WireClose::keep, end);
    expectResult(pending.input(frame("DISCONNECT", "invalid-header\n"), kAccept + 1ms),
                 {errorFrame("syntax")}, WireClose::after_flush, end);
}

enum class DrainOutcome { complete, handshakeDeadline, flushDeadline };

void checkTcpDeadlines() {
    const auto fakeConnect = handshake("heart-beat:020,030\n");
    WireLimits heartbeatLimits;
    heartbeatLimits.heartbeat_send = 10ms;
    heartbeatLimits.heartbeat_receive = 40ms;
    {
        ManualClock clock;
        auto dependencies = BrokerServer::productionDependencies();
        dependencies.clock = [&] { return clock.now(); };
        BrokerServer server(heartbeatLimits, dependencies);
        const auto port = server.startLoopback();
        auto neighbor = connectPeer(port);
        ASSERT_TRUE(neighbor.has_value());
        ASSERT_TRUE(writePeer(*neighbor, kConnect));
        expectConnected(readFrame(*neighbor), "10,40");
        for (const auto& input : {handshake("heart-beat:20,30\n"), fakeConnect}) {
            auto peer = connectPeer(port);
            ASSERT_TRUE(peer.has_value());
            ASSERT_TRUE(writePeer(*peer, input));
            expectConnected(readFrame(*peer), "10,40");
            ASSERT_TRUE(clock.observeClockAtCurrentTime(server));
            clock.advance(30ms, server);
            EXPECT_EQ(readExactly(*peer, 1), std::optional<std::string>{"\n"});
            ASSERT_TRUE(clock.observeClockAtCurrentTime(server));
            clock.advance(50ms, server);
            expectClosed(*peer, errorFrame("heartbeat-timeout"));
        }
        finishNeighbor(*neighbor);
    }
    for (const auto prefix : {0u, 7u}) {
        ManualClock clock;
        ScriptedWriter writer;
        auto dependencies = BrokerServer::productionDependencies();
        dependencies.clock = [&] { return clock.now(); };
        dependencies.writer = [&](auto& socket, auto data) { return writer.write(socket, data); };
        BrokerServer server(quietLimits(), dependencies);
        const auto port = server.startLoopback();
        writer.allow(std::numeric_limits<std::size_t>::max());
        auto neighbor = connectPeer(port);
        ASSERT_TRUE(neighbor.has_value());
        ASSERT_TRUE(writePeer(*neighbor, kConnect));
        EXPECT_EQ(readFrame(*neighbor), kConnected);
        writer.allow(prefix);
        auto peer = connectPeer(port);
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, kConnect));
        ASSERT_TRUE(writer.waitBlocked(kConnected.substr(prefix)));
        clock.advance(quietLimits().handshake, server);
        expectClosed(*peer, kConnected.substr(0, prefix));
        writer.allow(std::numeric_limits<std::size_t>::max());
        server.notifyClockChanged();
        finishNeighbor(*neighbor);
    }
    {
        ManualClock clock;
        ScriptedWriter writer;
        auto dependencies = BrokerServer::productionDependencies();
        dependencies.clock = [&] { return clock.now(); };
        dependencies.writer = [&](auto& socket, auto data) { return writer.write(socket, data); };
        BrokerServer server(quietLimits(), dependencies);
        auto peer = connectPeer(server.startLoopback());
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, handshake("receipt:r\n") + frame("DISCONNECT")));
        ASSERT_TRUE(writer.waitBlocked(kConnected));
        writer.allow(std::numeric_limits<std::size_t>::max());
        server.notifyClockChanged();
        expectClosed(*peer, std::string(kConnected) + receipt("r"));
    }
    for (auto outcome : {DrainOutcome::complete, DrainOutcome::handshakeDeadline, DrainOutcome::flushDeadline}) {
        // Waiting until the actual ERROR reaches the writer observes the transition into drain.
        WireLimits limits = quietLimits();
        limits.handshake = 100ms;
        limits.flush = outcome == DrainOutcome::flushDeadline ? 40ms : 200ms;
        ManualClock clock;
        ScriptedWriter writer;
        const auto broken = frame("DISCONNECT", "invalid-header\n");
        writer.watchInboundOnFirstWrite(broken.size());
        auto dependencies = BrokerServer::productionDependencies();
        dependencies.clock = [&] { return clock.now(); };
        dependencies.writer = [&](auto& socket, auto data) { return writer.write(socket, data); };
        BrokerServer server(limits, dependencies);
        auto peer = connectPeer(server.startLoopback());
        ASSERT_TRUE(peer.has_value());
        ASSERT_TRUE(writePeer(*peer, kConnect));
        ASSERT_TRUE(writer.waitFirstWrite());
        ASSERT_TRUE(writePeer(*peer, broken));
        ASSERT_TRUE(writer.waitInboundConsumed());
        if (outcome == DrainOutcome::complete) {
            writer.allow(std::numeric_limits<std::size_t>::max());
            server.notifyClockChanged();
            expectClosed(*peer, std::string(kConnected) + errorFrame("syntax"));
        } else {
            writer.allow(kConnected.size());
            server.notifyClockChanged();
            ASSERT_TRUE(writer.waitBlocked(errorFrame("syntax")));
            clock.advance(std::min(limits.handshake, limits.flush), server);
            expectClosed(*peer, kConnected);
        }
    }
}

void checkExpiredErrorDrain() {
    for (const bool heartbeat : {false, true}) {
        for (const bool flushExpires : {false, true}) {
            WireLimits limits = quietLimits();
            limits.handshake = 100ms;
            limits.flush = 40ms;
            limits.heartbeat_receive = 40ms;
            ManualClock clock;
            ScriptedWriter writer;
            auto dependencies = BrokerServer::productionDependencies();
            dependencies.clock = [&] { return clock.now(); };
            dependencies.writer = [&](auto& socket, auto data) { return writer.write(socket, data); };
            BrokerServer server(limits, dependencies);
            auto peer = connectPeer(server.startLoopback());
            ASSERT_TRUE(peer.has_value());
            const auto error = errorFrame(heartbeat ? "heartbeat-timeout" : "handshake-timeout");
            if (heartbeat) {
                writer.allow(frame("CONNECTED", "version:1.2\nheart-beat:0,40\n").size());
                ASSERT_TRUE(writePeer(*peer, handshake("heart-beat:20,0\n")));
                expectConnected(readFrame(*peer), "0,40");
            } else {
                ASSERT_TRUE(writePeer(*peer, "CONNECT\n"));
            }
            ASSERT_TRUE(clock.observeClockAtCurrentTime(server));
            clock.advance(heartbeat ? 80ms : limits.handshake, server);
            ASSERT_TRUE(writer.waitBlocked(error));
            if (flushExpires) {
                clock.advance(limits.flush, server);
                expectClosed(*peer, {});
            } else {
                writer.allow(std::numeric_limits<std::size_t>::max());
                server.notifyClockChanged();
                expectClosed(*peer, error);
            }
        }
    }
}

enum class BoundaryEventKind { bytes, timer, eof, failure, stop, written };

struct BoundaryEvent {
    BoundaryEventKind kind = BoundaryEventKind::timer;
    WireTime timestamp{};
    std::size_t budget = 0;
    std::string data;
    std::size_t written = 0;
    std::optional<WireFailureKind> failure;
};

struct ConnectionLog {
    WireTime accepted{};
    WireLimits limits;
    std::vector<BoundaryEvent> events;
    int destroyed = 0;
};

class BoundaryJournal {
public:
    std::size_t created(const WireLimits& limits, WireTime now) {
        const std::lock_guard lock(mutex_);
        records_.push_back({now, limits, {}, 0});
        changed_.notify_all();
        return records_.size() - 1;
    }

    void observed(std::size_t index, BoundaryEvent event) {
        const std::lock_guard lock(mutex_);
        records_[index].events.push_back(std::move(event));
        changed_.notify_all();
    }

    void destroyed(std::size_t index) {
        const std::lock_guard lock(mutex_);
        ++records_[index].destroyed;
        changed_.notify_all();
    }

    bool waitCreated(std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline, [&] { return records_.size() >= count; });
    }

    template<class Predicate>
    bool wait(std::size_t index, Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        std::unique_lock lock(mutex_);
        return changed_.wait_until(lock, deadline, [&] {
            return records_.size() > index && predicate(records_[index]);
        });
    }

    ConnectionLog record(std::size_t index) const {
        const std::lock_guard lock(mutex_);
        return records_.at(index);
    }

    std::size_t count() const {
        const std::lock_guard lock(mutex_);
        return records_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<ConnectionLog> records_;
};

bool hasEvent(const ConnectionLog& log, BoundaryEventKind kind, std::string_view data = {}) {
    return std::any_of(log.events.begin(), log.events.end(), [&](const BoundaryEvent& event) {
        return event.kind == kind && (data.empty() || event.data == data);
    });
}

std::size_t eventCount(const ConnectionLog& log, BoundaryEventKind kind) {
    return static_cast<std::size_t>(std::count_if(log.events.begin(), log.events.end(),
        [&](const BoundaryEvent& event) { return event.kind == kind; }));
}

WireResult boundaryOutput(std::initializer_list<std::string_view> output,
                          WireClose close = WireClose::keep,
                          std::optional<WireTime> deadline = std::nullopt) {
    WireResult result;
    result.close = close;
    result.deadline = deadline;
    for (const auto block : output) {
        result.output.push_back(owningBytes(block));
    }
    return result;
}

class WireBoundaryDouble final : public WireConnection {
public:
    WireBoundaryDouble(BoundaryJournal& journal, const WireLimits& limits, WireTime accepted)
        : journal_(journal), index_(journal.created(limits, accepted)), accepted_(accepted) {}

    ~WireBoundaryDouble() noexcept override {
        journal_.destroyed(index_);
    }

    WireResult handle(const WireInput& input) override {
        BoundaryEvent event;
        event.timestamp = input.now;
        event.budget = input.output_budget;
        if (const auto* incoming = std::get_if<WireBytes>(&input.event)) {
            event.kind = BoundaryEventKind::bytes;
            event.data = octets(incoming->bytes);
            journal_.observed(index_, event);
            return respond(event);
        }
        if (std::holds_alternative<WireTimer>(input.event)) {
            journal_.observed(index_, event);
            deadline_ = input.now == accepted_ ? std::optional{accepted_ + 100ms} : std::nullopt;
            return boundaryOutput({}, WireClose::keep, deadline_);
        }
        if (const auto* written = std::get_if<WireWritten>(&input.event)) {
            event.kind = BoundaryEventKind::written;
            event.written = written->bytes;
            journal_.observed(index_, event);
            return boundaryOutput({}, WireClose::keep, deadline_);
        }
        if (const auto* failure = std::get_if<WireFailure>(&input.event)) {
            event.kind = BoundaryEventKind::failure;
            event.failure = failure->kind;
        } else if (std::holds_alternative<WireEof>(input.event)) {
            event.kind = BoundaryEventKind::eof;
        } else {
            event.kind = BoundaryEventKind::stop;
        }
        journal_.observed(index_, event);
        // Exercise the transport rule for an invalid terminal reply.
        return boundaryOutput({"discard me"}, WireClose::keep, input.now + 1ms);
    }

private:
    WireResult respond(const BoundaryEvent& event) {
        if (event.data == "A") {
            deadline_ = event.timestamp + 40ms;
            return boundaryOutput({"ABCDE", "FG"}, WireClose::keep, deadline_);
        }
        if (event.data == "B") {
            deadline_.reset();
            return boundaryOutput({"HI"});
        }
        if (event.data == "T") {
            deadline_ = event.timestamp + 10ms;
            return boundaryOutput({"T"}, WireClose::keep, deadline_);
        }
        if (event.data == "o") {
            return boundaryOutput({std::string(event.budget + 1, '!')});
        }
        if (event.data == "d") {
            return boundaryOutput({"done"}, WireClose::after_flush, event.timestamp + 20ms);
        }
        if (event.data == "!") {
            throw std::runtime_error("scripted handle exception");
        }
        if (event.data == "p") {
            return boundaryOutput({"P"}, WireClose::keep, deadline_);
        }
        if (event.data == "q") {
            return boundaryOutput({"OK"}, WireClose::keep, deadline_);
        }
        return boundaryOutput({}, WireClose::keep, deadline_);
    }

    BoundaryJournal& journal_;
    std::size_t index_;
    WireTime accepted_;
    std::optional<WireTime> deadline_;
};

BrokerDependencies boundaryDependencies(BoundaryJournal& journal, ManualClock& clock,
                                       ScriptedWriter& writer) {
    return {
        [&](const WireLimits& limits, WireTime accepted) {
            return std::make_unique<WireBoundaryDouble>(journal, limits, accepted);
        },
        [&] { return clock.now(); },
        [&](StreamSocket& socket, std::span<const std::byte> data) {
            return writer.write(socket, data);
        },
    };
}

bool peerExchange(StreamSocket& peer, std::string_view message, std::string_view response) {
    if (!writePeer(peer, message)) {
        return false;
    }
    const auto received = readExactly(peer, response.size());
    return received.has_value() && *received == response;
}

bool destroyedOnce(const ConnectionLog& log) {
    return log.destroyed == 1;
}

std::optional<BoundaryEvent> eventFor(const ConnectionLog& log, std::string_view data) {
    const auto found = std::find_if(log.events.begin(), log.events.end(), [&](const BoundaryEvent& event) {
        return event.kind == BoundaryEventKind::bytes && event.data == data;
    });
    if (found == log.events.end()) {
        return std::nullopt;
    }
    return *found;
}

void checkEmptyDependencies() {
    for (int missing = 0; missing < 3; ++missing) {
        SCOPED_TRACE(missing);
        int calls = 0;
        BrokerDependencies dependencies{
            [&](const WireLimits&, WireTime) -> std::unique_ptr<WireConnection> {
                ++calls;
                return nullptr;
            },
            [&] {
                ++calls;
                return kAccept;
            },
            [&](StreamSocket&, std::span<const std::byte>) {
                ++calls;
                return WireWriteResult{WireWriteStatus::failed, 0};
            },
        };
        if (missing == 0) {
            dependencies.factory = {};
        } else if (missing == 1) {
            dependencies.clock = {};
        } else {
            dependencies.writer = {};
        }
        EXPECT_THROW({ BrokerServer server(quietLimits(), dependencies); }, Poco::InvalidArgumentException);
        EXPECT_EQ(calls, 0);
    }
}

void checkBoundaryTraffic() {
    WireLimits limits = quietLimits();
    limits.frame_bytes = 8;
    limits.output_bytes = 9;
    BoundaryJournal journal;
    ManualClock clock;
    ScriptedWriter writer;
    BrokerServer server(limits, boundaryDependencies(journal, clock, writer));
    const auto port = server.startLoopback();
    writer.allow(std::numeric_limits<std::size_t>::max());
    auto neighbor = connectPeer(port);
    ASSERT_TRUE(neighbor.has_value());
    ASSERT_TRUE(peerExchange(*neighbor, "p", "P"));
    EXPECT_THROW(server.startLoopback(), Poco::IllegalStateException);
    ASSERT_TRUE(peerExchange(*neighbor, "p", "P"));
    auto peer = connectPeer(port);
    ASSERT_TRUE(peer.has_value());
    ASSERT_TRUE(journal.waitCreated(2));

    writer.allow(2);
    ASSERT_TRUE(writePeer(*peer, "A"));
    EXPECT_EQ(readExactly(*peer, 2), std::optional<std::string>{"AB"});
    ASSERT_TRUE(writer.waitBlocked("CDE"));
    ASSERT_TRUE(journal.wait(1, [](const auto& log) {
        return hasEvent(log, BoundaryEventKind::written);
    }));
    ASSERT_TRUE(writePeer(*peer, "B"));
    ASSERT_TRUE(journal.wait(1, [](const auto& log) {
        return hasEvent(log, BoundaryEventKind::bytes, "B");
    }));
    const auto partialLog = journal.record(1);
    ASSERT_GE(partialLog.events.size(), 4u);
    EXPECT_EQ(partialLog.events[0].kind, BoundaryEventKind::timer);
    EXPECT_EQ(partialLog.events[0].timestamp, partialLog.accepted);
    EXPECT_EQ(partialLog.limits.frame_bytes, 8u);
    EXPECT_EQ(partialLog.limits.output_bytes, 9u);
    const auto a = eventFor(partialLog, "A");
    const auto b = eventFor(partialLog, "B");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(a->budget, 9u);
    EXPECT_EQ(b->budget, 4u);
    EXPECT_GE(b->timestamp, a->timestamp);
    EXPECT_EQ(eventCount(partialLog, BoundaryEventKind::written), 1u);
    const auto written = std::find_if(partialLog.events.begin(), partialLog.events.end(),
        [](const BoundaryEvent& event) { return event.kind == BoundaryEventKind::written; });
    ASSERT_NE(written, partialLog.events.end());
    EXPECT_EQ(written->written, 2u);
    EXPECT_EQ(written->budget, 4u);

    writer.allow(std::numeric_limits<std::size_t>::max());
    server.notifyClockChanged();
    EXPECT_EQ(readExactly(*peer, 7), std::optional<std::string>{"CDEFGHI"});
    ASSERT_TRUE(peerExchange(*peer, "T", "T"));
    clock.advance(10ms, server);
    ASSERT_TRUE(journal.wait(1, [](const auto& log) {
        return eventCount(log, BoundaryEventKind::timer) == 2;
    }));
    auto updated = journal.record(1);
    EXPECT_EQ(updated.events.back().timestamp, kAccept + 10ms);
    EXPECT_EQ(updated.events.back().kind, BoundaryEventKind::timer);
    clock.advance(1ms, server);
    ASSERT_TRUE(peerExchange(*peer, "p", "P"));
    EXPECT_EQ(eventCount(journal.record(1), BoundaryEventKind::timer), 2u);

    const std::string input(40, 'z');
    ASSERT_TRUE(writePeer(*peer, input));
    ASSERT_TRUE(journal.wait(1, [](const auto& log) {
        std::size_t received = 0;
        for (const auto& event : log.events) {
            if (event.data.starts_with('z')) {
                received += event.data.size();
            }
        }
        return received >= 40;
    }));
    std::string received;
    for (const auto& event : journal.record(1).events) {
        if (event.data.starts_with('z')) {
            EXPECT_LE(event.data.size(), 8u);
            received += event.data;
        }
    }
    EXPECT_EQ(received, input);

    ASSERT_TRUE(writePeer(*peer, "o"));
    expectClosed(*peer, {});
    ASSERT_TRUE(journal.wait(1, destroyedOnce));
    EXPECT_EQ(journal.record(1).events.back().data, "o");
    ASSERT_TRUE(peerExchange(*neighbor, "p", "P"));
    EXPECT_EQ(journal.record(0).destroyed, 0);
    server.stop();
    EXPECT_EQ(eventCount(journal.record(0), BoundaryEventKind::stop), 1u);
    EXPECT_EQ(journal.record(0).destroyed, 1);
    expectClosed(*neighbor, {});
    EXPECT_FALSE(connectPeer(port).has_value());
    EXPECT_THROW(server.startLoopback(), Poco::IllegalStateException);
    server.stop();
    EXPECT_EQ(eventCount(journal.record(0), BoundaryEventKind::stop), 1u);
}

enum class BoundaryFailure { eof, read, write, invalidZero, invalidLength, invalidBlocked,
                             invalidFailed, throwWriter, throwHandle, nullFactory, throwFactory };

void checkBoundaryFailure(BoundaryFailure failure) {
    BoundaryJournal journal;
    ManualClock clock;
    ScriptedWriter writer;
    auto dependencies = boundaryDependencies(journal, clock, writer);
    if (failure == BoundaryFailure::nullFactory || failure == BoundaryFailure::throwFactory) {
        dependencies.factory = [&](const WireLimits& limits, WireTime accepted) -> std::unique_ptr<WireConnection> {
            if (journal.count() == 1) {
                if (failure == BoundaryFailure::throwFactory) {
                    throw std::runtime_error("scripted factory exception");
                }
                return nullptr;
            }
            return std::make_unique<WireBoundaryDouble>(journal, limits, accepted);
        };
    }
    BrokerServer server(quietLimits(), dependencies);
    const auto port = server.startLoopback();
    writer.allow(std::numeric_limits<std::size_t>::max());
    auto neighbor = connectPeer(port);
    ASSERT_TRUE(neighbor.has_value());
    ASSERT_TRUE(peerExchange(*neighbor, "p", "P"));
    auto peer = connectPeer(port);
    ASSERT_TRUE(peer.has_value());
    if (failure == BoundaryFailure::nullFactory || failure == BoundaryFailure::throwFactory) {
        expectClosed(*peer, {});
        EXPECT_EQ(journal.count(), 1u);
        EXPECT_TRUE(peerExchange(*neighbor, "p", "P"));
        return;
    }
    ASSERT_TRUE(journal.waitCreated(2));
    switch (failure) {
        case BoundaryFailure::eof:
            ASSERT_TRUE(writePeer(*peer, "partial"));
            peer->shutdownSend();
            break;
        case BoundaryFailure::read:
            peer->setLinger(true, 0);
            peer->close();
            break;
        case BoundaryFailure::throwHandle:
            ASSERT_TRUE(writePeer(*peer, "!"));
            break;
        default:
            if (failure == BoundaryFailure::invalidZero) {
                writer.fail({WireWriteStatus::progress, 0});
            } else if (failure == BoundaryFailure::invalidLength) {
                writer.fail({WireWriteStatus::progress, 10});
            } else if (failure == BoundaryFailure::invalidBlocked) {
                writer.fail({WireWriteStatus::would_block, 1});
            } else if (failure == BoundaryFailure::invalidFailed) {
                writer.fail({WireWriteStatus::failed, 1});
            } else {
                writer.fail({WireWriteStatus::failed, 0}, failure == BoundaryFailure::throwWriter);
            }
            ASSERT_TRUE(writePeer(*peer, "p"));
            break;
    }
    if (failure != BoundaryFailure::read) {
        expectClosed(*peer, {});
    }
    ASSERT_TRUE(journal.wait(1, destroyedOnce));
    const auto log = journal.record(1);
    if (failure == BoundaryFailure::eof) {
        EXPECT_TRUE(hasEvent(log, BoundaryEventKind::bytes, "partial"));
        EXPECT_EQ(eventCount(log, BoundaryEventKind::eof), 1u);
        EXPECT_EQ(log.events.back().kind, BoundaryEventKind::eof);
    } else if (failure == BoundaryFailure::read || failure == BoundaryFailure::write) {
        EXPECT_EQ(eventCount(log, BoundaryEventKind::failure), 1u);
        EXPECT_EQ(log.events.back().failure, failure == BoundaryFailure::read
                     ? WireFailureKind::read : WireFailureKind::write);
    }
    EXPECT_EQ(eventCount(log, BoundaryEventKind::written), 0u);
    writer.clearFailure();
    EXPECT_TRUE(peerExchange(*neighbor, "p", "P"));
}

void checkBoundaryShutdown(bool useDestructor, bool expireDrain) {
    BoundaryJournal journal;
    ManualClock clock;
    ScriptedWriter writer;
    auto server = std::make_unique<BrokerServer>(quietLimits(), boundaryDependencies(journal, clock, writer));
    const auto port = server->startLoopback();
    writer.allow(std::numeric_limits<std::size_t>::max());
    auto active = connectPeer(port);
    ASSERT_TRUE(active.has_value());
    ASSERT_TRUE(peerExchange(*active, "p", "P"));
    auto draining = connectPeer(port);
    ASSERT_TRUE(draining.has_value());
    ASSERT_TRUE(journal.waitCreated(2));
    writer.allow(0);
    ASSERT_TRUE(writePeer(*draining, "d"));
    ASSERT_TRUE(writer.waitBlocked("done"));

    if (expireDrain) {
        clock.advance(20ms, *server);
        expectClosed(*draining, {});
        ASSERT_TRUE(journal.wait(1, destroyedOnce));
        writer.allow(std::numeric_limits<std::size_t>::max());
        server->notifyClockChanged();
        EXPECT_TRUE(peerExchange(*active, "p", "P"));
    }
    if (useDestructor) {
        server.reset();
    } else {
        server->stop();
        server->stop();
    }
    expectClosed(*active, {});
    expectClosed(*draining, {});
    EXPECT_FALSE(connectPeer(port).has_value());
    const auto activeLog = journal.record(0);
    EXPECT_EQ(eventCount(activeLog, BoundaryEventKind::stop), 1u);
    EXPECT_EQ(activeLog.destroyed, 1);
    const auto drainingLog = journal.record(1);
    EXPECT_EQ(eventCount(drainingLog, BoundaryEventKind::stop), 0u);
    EXPECT_EQ(drainingLog.destroyed, 1);
    EXPECT_EQ(drainingLog.events.back().data, "d");
}

void checkBoundaryStress() {
    BoundaryJournal journal;
    ManualClock clock;
    ScriptedWriter writer;
    BrokerServer server(quietLimits(), boundaryDependencies(journal, clock, writer));
    const auto port = server.startLoopback();
    writer.allow(std::numeric_limits<std::size_t>::max());
    auto neighbor = connectPeer(port);
    ASSERT_TRUE(neighbor.has_value());
    ASSERT_TRUE(peerExchange(*neighbor, "p", "P"));

    std::set<std::uint64_t> descriptors;
    bool reused = false;
    for (std::size_t index = 1; index <= 256; ++index) {
        SCOPED_TRACE(index);
        auto shortLived = connectPeer(port);
        ASSERT_TRUE(shortLived.has_value());
        ASSERT_TRUE(peerExchange(*shortLived, "p", "P"));
        if (const auto descriptor = writer.descriptor(); descriptor.has_value()) {
            reused = !descriptors.insert(*descriptor).second || reused;
        }
        shortLived->shutdownSend();
        expectClosed(*shortLived, {});
        ASSERT_TRUE(journal.wait(index, destroyedOnce));
        const auto log = journal.record(index);
        EXPECT_EQ(eventCount(log, BoundaryEventKind::bytes), 1u);
        EXPECT_EQ(eventCount(log, BoundaryEventKind::written), 1u);
        EXPECT_EQ(eventCount(log, BoundaryEventKind::eof), 1u);
        EXPECT_EQ(log.events.back().kind, BoundaryEventKind::eof);
    }
    EXPECT_EQ(journal.count(), 257u);
    ::testing::Test::RecordProperty("descriptor_reuse", reused ? "observed" : "not_observed");
    std::cout << "[ REQ-73-1 ] 256 accept/close exchanges, descriptor reuse "
              << (reused ? "observed" : "not observed") << '\n';

    ASSERT_TRUE(peerExchange(*neighbor, "p", "P"));
    writer.capChunk(1);
    for (std::size_t index = 0; index < 1000; ++index) {
        ASSERT_TRUE(peerExchange(*neighbor, "q", "OK")) << index;
    }
    const auto log = journal.record(0);
    EXPECT_EQ(log.destroyed, 0);
    EXPECT_EQ(eventCount(log, BoundaryEventKind::bytes), 1002u);
    for (const auto& event : log.events) {
        if (event.kind == BoundaryEventKind::bytes) {
            EXPECT_EQ(event.budget, quietLimits().output_bytes);
        }
    }
}

// REQ-73-1
TEST(WireContractTest, CapabilityNegotiation) {
    BrokerServer defaultServer;
    auto defaultPeer = connectPeer(defaultServer.startLoopback());
    ASSERT_TRUE(defaultPeer.has_value());
    ASSERT_TRUE(writePeer(*defaultPeer, kConnect));
    const auto defaultReply = readFrame(*defaultPeer);
    ASSERT_TRUE(defaultReply.starts_with("CONNECTED\n")) << defaultReply;
    expectConnected(defaultReply, "10000,10000");
    ASSERT_NO_FATAL_FAILURE(finishNeighbor(*defaultPeer));
    defaultServer.stop();

    ASSERT_NO_FATAL_FAILURE(checkConfiguredLimits());
    ASSERT_NO_FATAL_FAILURE(checkDecoder());
    ASSERT_NO_FATAL_FAILURE(checkReceiptBudgets());
    ASSERT_NO_FATAL_FAILURE(checkDirectHeartbeat());
    ASSERT_NO_FATAL_FAILURE(checkTcpDeadlines());
    ASSERT_NO_FATAL_FAILURE(checkExpiredErrorDrain());
    ASSERT_NO_FATAL_FAILURE(checkFrameLimits());

    BrokerServer server(quietLimits());
    const auto port = server.startLoopback();
    ASSERT_NO_FATAL_FAILURE(checkPositivePeers(port));
    ASSERT_NO_FATAL_FAILURE(checkHandshakeErrors(port));
}

// REQ-73-1
TEST(WireContractTest, WireConnectionBoundary) {
    ASSERT_NO_FATAL_FAILURE(checkBoundaryTraffic());
    ASSERT_NO_FATAL_FAILURE(checkEmptyDependencies());
    for (const auto failure : {BoundaryFailure::eof, BoundaryFailure::read, BoundaryFailure::write,
                               BoundaryFailure::invalidZero, BoundaryFailure::invalidLength,
                               BoundaryFailure::invalidBlocked, BoundaryFailure::invalidFailed,
                               BoundaryFailure::throwWriter, BoundaryFailure::throwHandle,
                               BoundaryFailure::nullFactory, BoundaryFailure::throwFactory}) {
        SCOPED_TRACE(static_cast<int>(failure));
        ASSERT_NO_FATAL_FAILURE(checkBoundaryFailure(failure));
    }
    ASSERT_NO_FATAL_FAILURE(checkBoundaryShutdown(false, true));
    ASSERT_NO_FATAL_FAILURE(checkBoundaryShutdown(false, false));
    ASSERT_NO_FATAL_FAILURE(checkBoundaryShutdown(true, false));
    ASSERT_NO_FATAL_FAILURE(checkBoundaryStress());
}

std::optional<double> peakRssBytes() {
#if defined(_WIN32)
    return std::nullopt;
#else
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return std::nullopt;
    }
#if defined(__APPLE__)
    return static_cast<double>(usage.ru_maxrss);
#else
    return static_cast<double>(usage.ru_maxrss) * 1024.0;
#endif
#endif
}

void WireHandshakeProfile(benchmark::State& state) {
    constexpr std::size_t warmupCount = 1000;
    constexpr std::size_t measuredCount = 10000;
    const auto input = handshake("receipt:r\n");
    const auto connectedAndReceipt = std::string(kConnected) + receipt("r");
    BrokerServer server(quietLimits());
    std::uint16_t port{};
    try {
        port = server.startLoopback();
    } catch (const Poco::Exception&) {
        state.SkipWithError("WireHandshakeProfile could not start its loopback listener");
        return;
    }
    auto roundTrip = [&] {
        auto peer = connectPeer(port);
        if (!peer.has_value() || !peerExchange(*peer, input, connectedAndReceipt) ||
            !writePeer(*peer, kDisconnect)) {
            return false;
        }
        const auto response = readClosed(*peer);
        return response.closed && response.data == kReceipt;
    };

    std::vector<double> wallMicros;
    wallMicros.reserve(measuredCount);
    double elapsedSeconds = 0;
    double cpuNsPerOperation = 0;
    std::size_t errors = 0;
    for (auto iteration : state) {
        (void)iteration;
        state.PauseTiming();
        for (std::size_t index = 0; index < warmupCount; ++index) {
            if (!roundTrip()) {
                ++errors;
                break;
            }
        }
        state.ResumeTiming();
        if (errors != 0) {
            break;
        }

        const auto cpuStart = std::clock();
        const auto wallStart = std::chrono::steady_clock::now();
        for (std::size_t index = 0; index < measuredCount; ++index) {
            const auto start = std::chrono::steady_clock::now();
            if (!roundTrip()) {
                ++errors;
                break;
            }
            const auto elapsed = std::chrono::steady_clock::now() - start;
            wallMicros.push_back(std::chrono::duration<double, std::micro>(elapsed).count());
        }
        elapsedSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
        const auto cpuEnd = std::clock();
        if (cpuStart != static_cast<std::clock_t>(-1) && cpuEnd >= cpuStart) {
            cpuNsPerOperation = (static_cast<double>(cpuEnd - cpuStart) * 1e9) /
                (CLOCKS_PER_SEC * static_cast<double>(measuredCount));
        } else {
            ++errors;
        }
    }
    state.counters["errors"] = static_cast<double>(errors);
    state.counters["connections"] = static_cast<double>(wallMicros.size());
    if (errors != 0 || wallMicros.size() != measuredCount || elapsedSeconds <= 0) {
        state.SkipWithError("WireHandshakeProfile did not complete every handshake and disconnect");
        return;
    }
    const auto rss = peakRssBytes();
    if (!rss.has_value()) {
        state.SkipWithError("WireHandshakeProfile cannot observe peak RSS on this host");
        return;
    }
    std::sort(wallMicros.begin(), wallMicros.end());
    state.counters["cpu_ns_per_connection"] = cpuNsPerOperation;
    state.counters["connections_per_second"] = static_cast<double>(measuredCount) / elapsedSeconds;
    state.counters["p95_round_trip_us"] = wallMicros[measuredCount * 95 / 100 - 1];
    state.counters["p99_round_trip_us"] = wallMicros[measuredCount * 99 / 100 - 1];
    state.counters["peak_rss_bytes"] = *rss;
}

BENCHMARK(WireHandshakeProfile)->Iterations(1)->MeasureProcessCPUTime()->UseRealTime();

}  // namespace
