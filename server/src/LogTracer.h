//
// Created by Alexander Bychuk on 29.01.2022.
//

#ifndef TINY_MQ__LOGTRACER_H_
#define TINY_MQ__LOGTRACER_H_

#include <Poco/Logger.h>
#include <Poco/Optional.h>
namespace tiny_mq {
namespace log {
class Trace {
  Poco::Logger *_log;
  Poco::Optional<std::string> _func;
  int64_t _localCounter{0};
  Poco::Optional<std::string> _beg;
  Poco::Optional<std::string> _end;
  static thread_local std::atomic_int64_t _counter;

 public:
  explicit Trace(Poco::Logger *l, const std::string &func);
  Trace(const Trace &) = delete;
  Trace(Trace &&o) = delete;
  ~Trace() noexcept;
};
}  // namespace log
}  // namespace tiny_mq
#ifdef _MSC_VER
#define __PRETTY_FUNCTION__ __FUNCTION__
#endif
// TRACE: zero-cost when trace level is disabled — format string is only evaluated
// when the logger actually has trace level enabled.
#define TRACE(logger)                                                                             \
  Poco::Logger& _traceLog_ = (logger).get();                                                     \
  tiny_mq::log::Trace trace(                                                                     \
    _traceLog_.trace() ? &_traceLog_ : nullptr,                                                  \
    _traceLog_.trace()                                                                            \
      ? Poco::format("%s [%s:%?d]", std::string(__PRETTY_FUNCTION__), std::string(__FILE__), __LINE__) \
      : std::string{})

#define TRACE_MSG(logger, msg)                                                                    \
  Poco::Logger& _traceLog_ = (logger).get();                                                     \
  tiny_mq::log::Trace trace(                                                                     \
    _traceLog_.trace() ? &_traceLog_ : nullptr,                                                  \
    _traceLog_.trace()                                                                            \
      ? Poco::format("%s [%s:%?d] : %s", std::string(__PRETTY_FUNCTION__), std::string(__FILE__), __LINE__, (msg)) \
      : std::string{})
#endif  // TINY_MQ__LOGTRACER_H_
