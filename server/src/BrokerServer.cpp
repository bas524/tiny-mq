#include "BrokerServer.h"
#include "StompConnection.h"

#include <Poco/Exception.h>
#include <Poco/Net/NetException.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/SocketReactor.h>
#include <Poco/Observer.h>
#include <Poco/Timespan.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <deque>
#include <map>
#include <thread>
#include <utility>

namespace tiny_mq {
namespace {
using Millis = std::chrono::milliseconds;
void validate(const WireLimits& limits, const BrokerDependencies& dependencies) {
  const auto maxMillis = std::chrono::duration_cast<Millis>(WireTime::duration::max()).count();
  const auto fits = [&](Millis value, unsigned factor) {
    return value.count() >= 0 && value.count() <= maxMillis / factor;
  };
  if (!dependencies.factory || !dependencies.clock || !dependencies.writer ||
      !limits.frame_bytes || !limits.header_bytes || !limits.header_count || !limits.output_bytes ||
      limits.handshake.count() <= 0 || limits.flush.count() <= 0 ||
      !fits(limits.handshake, 1) || !fits(limits.flush, 1) ||
      !fits(limits.heartbeat_send, 1) || !fits(limits.heartbeat_receive, 2)) {
    throw Poco::InvalidArgumentException("Invalid wire configuration");
  }
}
WireTime flushDeadline(WireTime now, Millis delay) {
  const auto duration = std::chrono::duration_cast<WireTime::duration>(delay);
  return now > WireTime::max() - duration ? WireTime::max() : now + duration;
}
}

struct BrokerServer::Impl {
  struct Reactor final : Poco::Net::SocketReactor {
    Impl& owner;
    explicit Reactor(Impl& instance) : owner(instance) {}
    void run() override { owner.run(); }
    Poco::Net::PollSet::SocketModeMap poll(Poco::Timespan timeout) {
      return const_cast<Poco::Net::PollSet&>(getPollSet()).poll(timeout);
    }
    void ready(const Poco::Net::Socket& socket, int flags) {
      if (flags & Poco::Net::PollSet::POLL_READ) {
        dispatch(socket, static_cast<Poco::Net::SocketNotification*>(getReadableNotification()));
      }
      if (flags & Poco::Net::PollSet::POLL_WRITE) {
        dispatch(socket, static_cast<Poco::Net::SocketNotification*>(getWritableNotification()));
      }
      if (flags & Poco::Net::PollSet::POLL_ERROR) {
        dispatch(socket, static_cast<Poco::Net::SocketNotification*>(getErrorNotification()));
      }
    }
  };
  struct Registration {
    Impl& owner;
    const std::uint64_t generation;
    Poco::Net::StreamSocket socket;
    std::unique_ptr<WireConnection> protocol;
    std::deque<std::vector<std::byte>> output;
    std::size_t offset = 0;
    std::size_t pending = 0;
    std::optional<WireTime> deadline;
    bool draining = false;
    bool writeRegistered = false;
    bool readRegistered = false;
    bool errorRegistered = false;
    bool readReady = false;
    bool writeReady = false;
    bool errorReady = false;

    Registration(Impl& instance, std::uint64_t id, Poco::Net::StreamSocket accepted)
        : owner(instance), generation(id), socket(std::move(accepted)) {}
    void readable(Poco::Net::ReadableNotification* notification) {
      Poco::AutoPtr<Poco::Net::ReadableNotification> hold(notification);
      if (owner.live(generation, this)) { readReady = true; }
    }
    void writable(Poco::Net::WritableNotification* notification) {
      Poco::AutoPtr<Poco::Net::WritableNotification> hold(notification);
      if (owner.live(generation, this)) { writeReady = true; }
    }
    void failed(Poco::Net::ErrorNotification* notification) {
      Poco::AutoPtr<Poco::Net::ErrorNotification> hold(notification);
      if (owner.live(generation, this)) { errorReady = true; }
    }
  };

  WireLimits limits;
  BrokerDependencies dependencies;
  std::unique_ptr<Reactor> reactor;
  std::optional<Poco::Net::ServerSocket> listener;
  std::jthread worker;
  std::atomic<bool> stopping{false};
  bool started = false;
  std::uint64_t nextGeneration = 1;
  std::map<std::uint64_t, std::unique_ptr<Registration>> peers;
  bool acceptReady = false;

  Impl(WireLimits settings, BrokerDependencies injected)
      : limits(settings), dependencies(std::move(injected)) {}
  bool live(std::uint64_t generation, const Registration* object) const {
    const auto found = peers.find(generation);
    return found != peers.end() && found->second.get() == object;
  }
  void accepting(Poco::Net::ReadableNotification* notification) {
    Poco::AutoPtr<Poco::Net::ReadableNotification> hold(notification);
    acceptReady = true;
  }
  void watch(Registration& peer) {
    using namespace Poco::Net;
    if (!peer.draining && !peer.readRegistered) {
      reactor->addEventHandler(peer.socket, Poco::Observer<Registration, ReadableNotification>(peer, &Registration::readable));
      peer.readRegistered = true;
    } else if (peer.draining && peer.readRegistered) {
      reactor->removeEventHandler(peer.socket, Poco::Observer<Registration, ReadableNotification>(peer, &Registration::readable));
      peer.readRegistered = false;
    }
    if (!peer.errorRegistered) {
      reactor->addEventHandler(peer.socket, Poco::Observer<Registration, ErrorNotification>(peer, &Registration::failed));
      peer.errorRegistered = true;
    }
    if (peer.pending && !peer.writeRegistered) {
      reactor->addEventHandler(peer.socket, Poco::Observer<Registration, WritableNotification>(peer, &Registration::writable));
      peer.writeRegistered = true;
    } else if (!peer.pending && peer.writeRegistered) {
      reactor->removeEventHandler(peer.socket, Poco::Observer<Registration, WritableNotification>(peer, &Registration::writable));
      peer.writeRegistered = false;
    }
  }
  void close(std::uint64_t id) noexcept {
    const auto found = peers.find(id);
    if (found == peers.end()) { return; }
    auto& peer = *found->second;
    // Unregister before close/destruction. No descriptor lookup can select a new generation.
    try {
      using namespace Poco::Net;
      if (peer.readRegistered) {
        reactor->removeEventHandler(peer.socket, Poco::Observer<Registration, ReadableNotification>(peer, &Registration::readable));
      }
      if (peer.writeRegistered) {
        reactor->removeEventHandler(peer.socket, Poco::Observer<Registration, WritableNotification>(peer, &Registration::writable));
      }
      if (peer.errorRegistered) {
        reactor->removeEventHandler(peer.socket, Poco::Observer<Registration, ErrorNotification>(peer, &Registration::failed));
      }
    } catch (...) {}
    peer.deadline.reset();
    try { peer.socket.close(); } catch (...) {}
    peers.erase(found);
  }
  bool event(std::uint64_t id, WireEvent incoming, bool terminal = false,
             std::optional<WireTime> timestamp = std::nullopt) {
    auto& peer = *peers.at(id);
    if (peer.draining) { return true; }
    try {
      const auto now = timestamp ? *timestamp : dependencies.clock();
      auto result = peer.protocol->handle({now, limits.output_bytes - peer.pending, std::move(incoming)});
      if (terminal || result.close == WireClose::immediate) { close(id); return false; }
      std::size_t sum = 0;
      for (const auto& block : result.output) {
        if (block.size() > limits.output_bytes - peer.pending - sum) { close(id); return false; }
        sum += block.size();
      }
      peer.pending += sum;
      for (auto& block : result.output) {
        if (!block.empty()) { peer.output.push_back(std::move(block)); }
      }
      peer.deadline = result.deadline;
      if (result.close == WireClose::after_flush) {
        peer.draining = true;
        const auto drain = flushDeadline(now, limits.flush);
        if (!peer.deadline || drain < *peer.deadline) { peer.deadline = drain; }
        if (!peer.pending) { close(id); return false; }
      }
      watch(peer);
      return true;
    } catch (...) { close(id); return false; }
  }
  void accept() {
    try {
      auto socket = listener->acceptConnection();
      socket.setBlocking(false);
      socket.setNoDelay(true);
      const auto now = dependencies.clock();
      auto protocol = dependencies.factory(limits, now);
      if (!protocol) { socket.close(); return; }
      const auto id = nextGeneration++;
      auto peer = std::make_unique<Registration>(*this, id, std::move(socket));
      peer->protocol = std::move(protocol);
      peers.emplace(id, std::move(peer));
      event(id, WireTimer{}, false, now);
    } catch (...) { /* Accepted socket and failed factory remain local to this accept. */ }
  }
  void read(std::uint64_t id, bool pollError) {
    auto& peer = *peers.at(id);
    try {
      std::vector<std::byte> buffer(std::min(limits.frame_bytes, std::size_t{65536}));
      const int count = peer.socket.receiveBytes(buffer.data(), static_cast<int>(buffer.size()));
      if (count == 0) { event(id, WireEof{}, true); }
      else if (count > 0) {
        event(id, WireBytes{std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(count))});
      } else if (pollError || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        event(id, WireFailure{WireFailureKind::read}, true);
      }
    } catch (...) { event(id, WireFailure{WireFailureKind::read}, true); }
  }
  void write(std::uint64_t id) {
    auto& peer = *peers.at(id);
    if (!peer.pending) { return; }
    try {
      // A due timer always wins over a new write attempt, including drain.
      const auto now = dependencies.clock();
      if (peer.deadline && now >= *peer.deadline) { return; }
      const auto block = std::span<const std::byte>(peer.output.front()).subspan(peer.offset);
      const auto result = dependencies.writer(peer.socket, block);
      if (result.status == WireWriteStatus::would_block && result.bytes == 0) { return; }
      if (result.status != WireWriteStatus::progress || result.bytes == 0 || result.bytes > block.size()) {
        if (peer.draining) { close(id); } else { event(id, WireFailure{WireFailureKind::write}, true); }
        return;
      }
      peer.offset += result.bytes;
      peer.pending -= result.bytes;
      if (peer.offset == peer.output.front().size()) { peer.output.pop_front(); peer.offset = 0; }
      if (peer.draining) {
        if (!peer.pending) { close(id); } else { watch(peer); }
      } else { event(id, WireWritten{result.bytes}); }
    } catch (...) { close(id); }
  }
  void shutdown() noexcept {
    if (listener) {
      try {
        reactor->removeEventHandler(*listener, Poco::Observer<Impl, Poco::Net::ReadableNotification>(*this, &Impl::accepting));
        listener->close();
      } catch (...) {}
      listener.reset();
    }
    while (!peers.empty()) {
      const auto id = peers.begin()->first;
      if (!peers.begin()->second->draining) { event(id, WireStop{}, true); }
      else { close(id); }
    }
  }
  void run() noexcept {
    try {
      while (!stopping.load()) {
        auto wait = std::chrono::microseconds(1000000);
        const auto now = dependencies.clock();
        for (const auto& [id, peer] : peers) {
          static_cast<void>(id);
          if (peer->deadline) {
            const auto delay = *peer->deadline <= now ? std::chrono::microseconds(0) :
                std::chrono::duration_cast<std::chrono::microseconds>(*peer->deadline - now);
            wait = std::min(wait, delay);
          }
        }
        const auto ready = reactor->poll(Poco::Timespan(wait.count()));
        if (stopping.load()) { break; }
        // Dispatch only marks readiness. All reads precede all timers, then writes.
        for (const auto& [socket, flags] : ready) { reactor->ready(socket, flags); }
        std::vector<std::uint64_t> ids;
        for (const auto& [id, peer] : peers) { static_cast<void>(peer); ids.push_back(id); }
        for (auto id : ids) {
          if (!peers.contains(id)) { continue; }
          auto& peer = *peers.at(id);
          const bool errorReady = std::exchange(peer.errorReady, false);
          const bool readReady = std::exchange(peer.readReady, false);
          if (peer.draining) {
            if (errorReady) { close(id); }
          } else if (readReady || errorReady) {
            // Poco can report a hangup as an error while read bytes or EOF are pending.
            // The result of recv distinguishes buffered data, orderly EOF and a reset.
            read(id, errorReady);
          }
        }
        for (auto id : ids) {
          if (!peers.contains(id)) { continue; }
          auto& peer = *peers.at(id);
          if (peer.deadline && dependencies.clock() >= *peer.deadline) {
            if (peer.draining) { close(id); } else { event(id, WireTimer{}); }
          }
        }
        for (auto id : ids) {
          if (peers.contains(id) && std::exchange(peers.at(id)->writeReady, false)) { write(id); }
        }
        // Accept after handling the old readiness snapshot prevents descriptor aliasing.
        if (std::exchange(acceptReady, false) && !stopping.load()) { accept(); }
      }
    } catch (...) {}
    shutdown();
  }
};

BrokerServer::BrokerServer(WireLimits limits) : BrokerServer(limits, productionDependencies()) {}
BrokerServer::BrokerServer(WireLimits limits, BrokerDependencies dependencies) {
  validate(limits, dependencies);
  impl_ = std::make_unique<Impl>(limits, std::move(dependencies));
}
BrokerDependencies BrokerServer::productionDependencies() {
  return {
      [](const WireLimits& limits, WireTime now) { return std::make_unique<StompConnection>(limits, now); },
      [] { return std::chrono::steady_clock::now(); },
      [](Poco::Net::StreamSocket& socket, std::span<const std::byte> bytes) {
        try {
          const auto size = static_cast<int>(std::min(bytes.size(), static_cast<std::size_t>(INT_MAX)));
          const int count = socket.sendBytes(bytes.data(), size);
          if (count > 0) { return WireWriteResult{WireWriteStatus::progress, static_cast<std::size_t>(count)}; }
          if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return WireWriteResult{WireWriteStatus::would_block, 0};
          }
        } catch (const Poco::TimeoutException&) { return WireWriteResult{WireWriteStatus::would_block, 0}; }
        catch (...) { return WireWriteResult{WireWriteStatus::failed, 0}; }
        return WireWriteResult{WireWriteStatus::failed, 0};
      }};
}
BrokerServer::~BrokerServer() noexcept { stop(); }
std::uint16_t BrokerServer::startLoopback() {
  if (impl_->started || impl_->stopping.load()) { throw Poco::IllegalStateException("Loopback can only be started once"); }
  impl_->reactor = std::make_unique<Impl::Reactor>(*impl_);
  impl_->listener.emplace(Poco::Net::SocketAddress("127.0.0.1", 0));
  impl_->listener->setBlocking(false);
  const auto port = impl_->listener->address().port();
  impl_->reactor->addEventHandler(*impl_->listener,
      Poco::Observer<Impl, Poco::Net::ReadableNotification>(*impl_, &Impl::accepting));
  impl_->started = true;
  impl_->worker = std::jthread([this] { impl_->reactor->run(); });
  return port;
}
void BrokerServer::notifyClockChanged() { if (impl_->reactor) { impl_->reactor->wakeUp(); } }
void BrokerServer::stop() noexcept {
  impl_->stopping.store(true);
  if (impl_->reactor) { impl_->reactor->wakeUp(); }
  if (impl_->worker.joinable()) { impl_->worker.join(); }
}
}  // namespace tiny_mq
