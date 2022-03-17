//
// Created by Alexander Bychuk on 29.01.2022.
//

#include "LogTracer.h"
#include <Poco/String.h>
namespace tiny_mq {
namespace log {
thread_local std::atomic_int64_t Trace::_counter = {0};
Trace::Trace(Poco::Logger *l, const std::string &func)
    : _log(l), _func(func), _localCounter(_counter.load()), _beg(std::string(_localCounter, '>')), _end(_beg) {
  if (_log) {
    _counter++;
    _log->trace("%sbeg %s", _beg.value(), _func.value());
  }
}
Trace::~Trace() noexcept {
  if (_log) {
    _log->trace("%send %s", _end.value(), _func.value());
    _counter--;
  }
}
}  // namespace log
}  // namespace tiny_mq