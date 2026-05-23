//
// Created by Alexander Bychuk on 12.04.2026.
//

#ifndef TINY_MQ__SELECTOR_H_
#define TINY_MQ__SELECTOR_H_

#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

namespace tiny_mq {

class Message;

/// Thrown by Selector::parse() when the expression is syntactically invalid.
class InvalidSelectorException : public std::runtime_error {
 public:
  explicit InvalidSelectorException(const std::string &msg) : std::runtime_error("Invalid selector: " + msg) {}
};

/// Runtime value produced during selector evaluation.
/// monostate represents SQL NULL.
using SelectorValue = std::variant<std::monostate, bool, int64_t, double, std::string>;

/// Compiled JMS SQL-92 message selector.
///
/// Usage:
///   auto sel = Selector::parse("rank > 100 AND name = 'James'");
///   if (sel->matches(message)) { ... }
class Selector {
 public:
  /// Parse a selector expression; throws InvalidSelectorException on error.
  /// An empty expression is valid and matches every message.
  static std::shared_ptr<Selector> parse(const std::string &expression);

  /// Returns true if the message satisfies this selector.
  bool matches(const Message &message) const;

  ~Selector();

 private:
  struct Impl;
  std::unique_ptr<Impl> _impl;
  explicit Selector(std::unique_ptr<Impl> impl);
};

}  // namespace tiny_mq

#endif  // TINY_MQ__SELECTOR_H_
