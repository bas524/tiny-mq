#include "SessionDispatch.h"
#include "Connection.h"
#include "Exceptions.h"

namespace tiny_mq {
thread_local SessionDispatch* SessionDispatch::current = nullptr;

thread_local bool SessionDispatch::inCompletion = false;

std::shared_ptr<SessionDispatch::Completion> SessionDispatch::reserve(
    Producer& producer, std::function<void()> invoke) {
  auto item = std::make_shared<Completion>(Completion{&producer, std::move(invoke), false, std::this_thread::get_id()});
  std::lock_guard lock(_mutex);
  if (_stopping || producer._closed) throw IllegalStateException("producer or session is closed");
  if (!_completionWorker.joinable()) _completionWorker = std::thread(&SessionDispatch::runCompletions, this);
  _completions.push_back(item);
  return item;
}

void SessionDispatch::complete(const std::shared_ptr<Completion>& completion) {
  std::lock_guard lock(_mutex);
  completion->ready = true;
  _changed.notify_all();
}

void SessionDispatch::signalListener() {
  if (_notification) {
    _notification->fetch_add(1, std::memory_order_release);
    _notification->notify_all();
  }
}

void SessionDispatch::add(const std::shared_ptr<Consumer>& consumer) {
  std::lock_guard lock(_mutex);
  if (_stopping) throw IllegalStateException("session is closed");
  if (!_notification) _notification = std::make_shared<std::atomic_uint64_t>();
  if (!_listenerWorker.joinable()) _listenerWorker = std::thread(&SessionDispatch::runListeners, this);
  const bool watched = std::any_of(_listeners.begin(), _listeners.end(), [&](const auto& weak) {
    auto existing = weak.lock();
    return existing && existing->_queue == consumer->_queue;
  });
  _listeners.emplace_back(consumer);
  try {
    if (!watched) consumer->_queue->watch(_notification);
  } catch (...) {
    _listeners.pop_back();
    throw;
  }
  _hasListeners = true;
  signalListener();
}

void SessionDispatch::remove(const Consumer& consumer) {
  std::lock_guard lock(_mutex);
  bool watched = false;
  _listeners.remove_if([&](const auto& weak) {
    auto item = weak.lock();
    if (!item || item.get() == &consumer) return true;
    watched |= item->_queue == consumer._queue;
    return false;
  });
  if (!watched && _notification) consumer._queue->unwatch(_notification);
  _hasListeners = !_listeners.empty() || _activeConsumer;
}

void SessionDispatch::wait(const Producer* producer) {
  std::unique_lock lock(_mutex);
  if (current == this && inCompletion && _activeProducer && (!producer || producer == _activeProducer)) {
    throw IllegalStateException("operation unavailable from completion callback");
  }
  _changed.wait(lock, [&] {
    if (_activeProducer && (!producer || producer == _activeProducer)) return false;
    return std::none_of(_completions.begin(), _completions.end(),
                        [&](const auto& item) { return !producer || producer == item->producer; });
  });
}

void SessionDispatch::wait(const Consumer& consumer) {
  if (current == this && !inCompletion) return;
  std::unique_lock lock(_mutex);
  _changed.wait(lock, [&] { return _activeConsumer != &consumer; });
}

void SessionDispatch::shutdown() {
  {
    std::lock_guard lock(_mutex);
    _stopping = true;
    _listeners.clear();
    signalListener();
    _changed.notify_all();
  }
  if (_listenerWorker.joinable()) _listenerWorker.join();
  if (_completionWorker.joinable()) _completionWorker.join();
}

void SessionDispatch::runCompletions() {
  std::unique_lock lock(_mutex);
  current = this;
  inCompletion = true;
  for (;;) {
    _changed.wait(lock, [&] {
      return _completions.empty() ? _stopping : _completions.front()->ready;
    });
    if (_completions.empty()) break;
    auto completion = std::move(_completions.front());
    _completions.pop_front();
    _activeProducer = completion->producer;
    lock.unlock();
    const auto invoke = [&] {
      current = this;
      inCompletion = true;
      try {
        completion->invoke();
      } catch (...) {
        // The outcome was already delivered to the application.
      }
    };
    if (completion->caller == std::this_thread::get_id()) {
      // A reentrant send's callback has to run on a different thread too.
      std::thread alternate(invoke);
      alternate.join();
    } else {
      invoke();
    }
    lock.lock();
    _activeProducer = nullptr;
    _changed.notify_all();
  }
  current = nullptr;
}

void SessionDispatch::runListeners() {
  std::unique_lock lock(_mutex);
  current = this;
  auto& connection = _session.connection();
  while (!_stopping) {
    const auto generation = _notification->load(std::memory_order_acquire);
    bool delivered = false;
    if (!_stopping && !_listeners.empty() && connection.isStarted()) {
      // Rotate the starting consumer so a busy queue cannot starve the rest.
      const auto count = _listeners.size();
      for (size_t i = 0; i < count && !_listeners.empty() && !_stopping; ++i) {
        auto consumer = _listeners.front().lock();
        _listeners.splice(_listeners.end(), _listeners, _listeners.begin());
        if (!consumer) continue;
        _activeConsumer = consumer.get();
        consumer->_callbackActive = true;
        lock.unlock();
        if (connection.enterListener()) {
          try {
            delivered = consumer->dispatchOne();
          } catch (const std::exception& error) {
            poco_error(_session._logger.get(), std::string("listener delivery: ") + error.what());
          } catch (...) {
            poco_error(_session._logger.get(), "listener delivery failed");
          }
          connection.leaveListener();
        }
        lock.lock();
        consumer->_callbackActive = false;
        consumer->_callbackActive.notify_all();
        _activeConsumer = nullptr;
        _hasListeners = !_listeners.empty();
        _changed.notify_all();
        if (delivered) break;
      }
    }
    if (!delivered && !_stopping) {
      lock.unlock();
      _notification->wait(generation, std::memory_order_acquire);
      lock.lock();
    }
  }
  current = nullptr;
}
}
