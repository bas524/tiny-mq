//
// JMS-style exception hierarchy for tiny-mq.
//
// Kept header-only and dependency-free (std only) so any layer can throw these
// without pulling in Poco.  More JMS exception kinds (e.g. InvalidSelector,
// MessageFormat) are added here as the corresponding specs land.
//

#ifndef TINY_MQ__EXCEPTIONS_H_
#define TINY_MQ__EXCEPTIONS_H_

#include <stdexcept>
#include <string>

namespace tiny_mq {

// Thrown when a method is invoked in a state that does not allow it — most
// notably any operation on a Connection (or Session) after close(), and a
// second call to setClientID().  Mirrors jakarta.jms.IllegalStateException.
class IllegalStateException : public std::logic_error {
 public:
  explicit IllegalStateException(const std::string &what) : std::logic_error(what) {}
};

}  // namespace tiny_mq

#endif  // TINY_MQ__EXCEPTIONS_H_
