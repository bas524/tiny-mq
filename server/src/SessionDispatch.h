#ifndef TINY_MQ_SESSION_DISPATCH_H
#define TINY_MQ_SESSION_DISPATCH_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

namespace tiny_mq {
class Connection;
class Session;
class Consumer;
class Producer;

// Internal session callback executor. A reserved completion is installed before
// send work begins, so acceptance already owns its slot and its close barrier.
class SessionDispatch {
  friend class Connection;
  friend class Session;
  friend class Consumer;
  friend class Producer;

  struct Completion {
    Producer* producer;
    std::function<void()> invoke;
    bool ready = false;
    std::thread::id caller;
  };

  explicit SessionDispatch(Session& session) : _session(session) {}
  std::shared_ptr<Completion> reserve(Producer& producer, std::function<void()> invoke);
  void complete(const std::shared_ptr<Completion>& completion);
  void add(const std::shared_ptr<Consumer>& consumer);
  void remove(const Consumer& consumer);
  void wait(const Producer* producer = nullptr);
  void wait(const Consumer& consumer);
  void shutdown();
  void runCompletions();
  void runListeners();
  void signalListener();
  bool hasListeners() const { return _hasListeners.load(std::memory_order_acquire); }

  static thread_local SessionDispatch* current;
  static thread_local bool inCompletion;
  Producer* _activeProducer = nullptr;
  Consumer* _activeConsumer = nullptr;
  Session& _session;
  std::mutex _mutex;
  std::condition_variable _changed;
  std::list<std::weak_ptr<Consumer>> _listeners;
  std::atomic_bool _hasListeners{false};
  std::shared_ptr<std::atomic_uint64_t> _notification;
  std::deque<std::shared_ptr<Completion>> _completions;
  bool _stopping = false;
  std::thread _completionWorker;
  std::thread _listenerWorker;
};
}
#endif
