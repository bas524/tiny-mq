//
// Created by Alexander Bychuk on 12.04.2026.
//

#include "Selector.h"
#include "Message.h"

#include <Poco/UTF8Encoding.h>
#include <Poco/Unicode.h>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <functional>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace tiny_mq {
namespace {

enum class TokenType {
  End, Identifier, String, Number, True, False, Null, And, Or, Not, Like, Escape,
  Between, In, Is, Eq, Ne, Lt, Le, Gt, Ge, Left, Right, Comma, Plus, Minus, Multiply, Divide
};

struct Token {
  TokenType type;
  std::string text;
};

bool digit(char ch) { return ch >= '0' && ch <= '9'; }

int unicodeAt(std::string_view input, size_t& pos) {
  if (pos == input.size()) return -1;
  auto first = static_cast<unsigned char>(input[pos]);
  if (first < 128) {
    ++pos;
    return first;
  }
  static const Poco::UTF8Encoding encoding;
  int size = encoding.sequenceLength(reinterpret_cast<const unsigned char*>(input.data() + pos), input.size() - pos);
  if (size < 1 || static_cast<size_t>(size) > input.size() - pos) return -1;
  int codepoint = encoding.convert(reinterpret_cast<const unsigned char*>(input.data() + pos));
  if (codepoint >= 0) pos += size;
  return codepoint;
}

bool javaIdentifier(int ch, bool first) {
  if (ch < 0) return false;
  if (ch < 128) {
    if (ch == '_' || ch == '$' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) return true;
    return !first && (digit(static_cast<char>(ch)) || ch < 9 || (ch >= 14 && ch <= 27) || ch == 127);
  }
  Poco::Unicode::CharacterProperties props{};
  Poco::Unicode::properties(ch, props);
  using U = Poco::Unicode;
  if (props.category == U::UCP_LETTER || props.type == U::UCP_LETTER_NUMBER ||
      props.type == U::UCP_CURRENCY_SYMBOL || props.type == U::UCP_CONNECTOR_PUNCTUATION) return true;
  return !first && (props.type == U::UCP_DECIMAL_NUMBER || props.type == U::UCP_SPACING_MARK ||
                    props.type == U::UCP_NON_SPACING_MARK || props.type == U::UCP_FORMAT || ch <= 159);
}

class Tokenizer {
 public:
  explicit Tokenizer(std::string_view text) : input(text) {}

  Token next() {
    while (pos < input.size() && std::string_view(" \t\f\r\n").find(input[pos]) != std::string_view::npos) ++pos;
    if (pos == input.size()) return {TokenType::End, {}};
    const size_t begin = pos;
    char ch = input[pos++];
    using T = TokenType;
    switch (ch) {
      case '(': return {T::Left, "("};
      case ')': return {T::Right, ")"};
      case ',': return {T::Comma, ","};
      case '+': return {T::Plus, "+"};
      case '-': return {T::Minus, "-"};
      case '*': return {T::Multiply, "*"};
      case '/': return {T::Divide, "/"};
      case '=': return {T::Eq, "="};
      case '<':
        if (accept('=')) return {T::Le, "<="};
        if (accept('>')) return {T::Ne, "<>"};
        return {T::Lt, "<"};
      case '>':
        if (accept('=')) return {T::Ge, ">="};
        return {T::Gt, ">"};
      case '\'': {
        std::string value;
        while (pos < input.size()) {
          ch = input[pos++];
          if (ch == '\'' && !accept('\'')) return {T::String, std::move(value)};
          value.push_back(ch);
        }
        throw InvalidSelectorException("unterminated string");
      }
      default: break;
    }

    if (digit(ch) || (ch == '.' && pos < input.size() && digit(input[pos]))) {
      bool hex = ch == '0' && pos < input.size() && (input[pos] == 'x' || input[pos] == 'X');
      if (hex) ++pos;
      while (pos < input.size()) {
        const char c = input[pos];
        if (digit(c) || c == '.' || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
          ++pos;
        } else if ((c == '+' || c == '-') &&
                   std::string_view(hex ? "pP" : "eE").find(input[pos - 1]) != std::string_view::npos) {
          ++pos;
        } else {
          break;
        }
      }
      return {T::Number, std::string(input.substr(begin, pos - begin))};
    }

    pos = begin;
    if (!javaIdentifier(unicodeAt(input, pos), true)) {
      throw InvalidSelectorException("unexpected character at byte " + std::to_string(begin));
    }
    for (size_t next = pos; javaIdentifier(unicodeAt(input, next), false);) pos = next;
    std::string word(input.substr(begin, pos - begin));
    std::string upper = word;
    for (char& c : upper) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    constexpr std::pair<std::string_view, T> keywords[] = {
        {"TRUE", T::True}, {"FALSE", T::False}, {"NULL", T::Null}, {"NOT", T::Not},
        {"AND", T::And}, {"OR", T::Or}, {"BETWEEN", T::Between}, {"LIKE", T::Like},
        {"IN", T::In}, {"IS", T::Is}, {"ESCAPE", T::Escape}};
    for (auto [name, type] : keywords) if (upper == name) return {type, std::move(word)};
    return {T::Identifier, std::move(word)};
  }

 private:
  bool accept(char ch) {
    if (pos == input.size() || input[pos] != ch) return false;
    ++pos;
    return true;
  }

  std::string_view input;
  size_t pos = 0;
};

struct Incompatible {};
using Value = std::variant<std::monostate, bool, int32_t, int64_t, float, double, std::string_view, Incompatible>;
using Eval = std::function<Value(const Message&)>;
using T = TokenType;

bool isNull(const Value& value) { return std::holds_alternative<std::monostate>(value); }

bool isNumeric(const Value& value) {
  return value.index() >= 2 && value.index() <= 5;
}

template<class R>
R numeric(const Value& value) {
  switch (value.index()) {
    case 2: return static_cast<R>(std::get<int32_t>(value));
    case 3: return static_cast<R>(std::get<int64_t>(value));
    case 4: return static_cast<R>(std::get<float>(value));
    default: return static_cast<R>(std::get<double>(value));
  }
}

template<class Operation>
Value promote(const Value& left, const Value& right, Operation operation) {
  switch (std::max(left.index(), right.index())) {
    case 2: return operation(numeric<int32_t>(left), numeric<int32_t>(right));
    case 3: return operation(numeric<int64_t>(left), numeric<int64_t>(right));
    case 4: return operation(numeric<float>(left), numeric<float>(right));
    default: return operation(numeric<double>(left), numeric<double>(right));
  }
}

template<class V>
bool compare(V left, V right, T op) {
  switch (op) {
    case T::Eq: return left == right;
    case T::Ne: return left != right;
    case T::Lt: return left < right;
    case T::Le: return left <= right;
    case T::Gt: return left > right;
    case T::Ge: return left >= right;
    default: return false;
  }
}

Value compareValues(const Value& left, const Value& right, T op) {
  if (isNull(left) || isNull(right)) return {};
  if (isNumeric(left) && isNumeric(right)) {
    return promote(left, right, [op](auto a, auto b) { return compare(a, b, op); });
  }
  if (op == T::Eq || op == T::Ne) {
    if (const auto* a = std::get_if<std::string_view>(&left)) {
      if (const auto* b = std::get_if<std::string_view>(&right)) return compare(*a, *b, op);
    }
    if (const auto* a = std::get_if<bool>(&left)) {
      if (const auto* b = std::get_if<bool>(&right)) return compare(*a, *b, op);
    }
  }
  return false;
}

template<class V>
Value arithmetic(V a, V b, T op) {
  if constexpr (std::is_integral_v<V>) {
    if (op == T::Divide) {
      if (b == 0) return {};
      if (a == std::numeric_limits<V>::min() && b == -1) return a;
      return a / b;
    }
    // Java integer arithmetic wraps, while signed overflow in C++ is UB.
    using U = std::make_unsigned_t<V>;
    const auto x = static_cast<U>(a), y = static_cast<U>(b);
    if (op == T::Multiply) return std::bit_cast<V>(x * y);
    if (op == T::Minus) return std::bit_cast<V>(x - y);
    return std::bit_cast<V>(x + y);
  } else {
    if (op == T::Divide) return a / b;
    if (op == T::Multiply) return a * b;
    if (op == T::Minus) return a - b;
    return a + b;
  }
}

Value calculate(const Value& a, const Value& b, T op) {
  if (isNull(a) || isNull(b)) return {};
  if (!isNumeric(a) || !isNumeric(b)) return Incompatible{};
  return promote(a, b, [op](auto x, auto y) { return arithmetic(x, y, op); });
}

SelectorTruth truth(const Value& value) {
  if (auto* result = std::get_if<bool>(&value)) return *result ? SelectorTruth::True : SelectorTruth::False;
  return SelectorTruth::Unknown;
}

Value negate(const Value& value) {
  if (const auto* b = std::get_if<bool>(&value)) return !*b;
  return {};
}

Value logical(const Value& left, const Value& right, bool shortCircuitValue) {
  if (const auto* lhs = std::get_if<bool>(&left); lhs && *lhs == shortCircuitValue) return shortCircuitValue;
  if (const auto* rhs = std::get_if<bool>(&right); rhs && *rhs == shortCircuitValue) return shortCircuitValue;
  if (std::holds_alternative<bool>(left) && std::holds_alternative<bool>(right)) return !shortCircuitValue;
  return {};
}

Value propertyValue(const Message& message, const std::string& name) {
  const auto* property = message.findProperty(name);
  if (!property) return {};
  auto extract = [&]<class Stored, class Result>() -> std::optional<Value> {
    if (const auto* value = Poco::AnyCast<Stored>(property)) {
      if (value->isNull()) return Value{};
      return Value(static_cast<Result>(value->value()));
    }
    return std::nullopt;
  };
  if (auto v = extract.template operator()<property::Int, int32_t>()) return *v;
  if (auto v = extract.template operator()<property::String, std::string_view>()) return *v;
  if (auto v = extract.template operator()<property::Long, int64_t>()) return *v;
  if (auto v = extract.template operator()<property::Bool, bool>()) return *v;
  if (auto v = extract.template operator()<property::Byte, int32_t>()) return *v;
  if (auto v = extract.template operator()<property::Short, int32_t>()) return *v;
  if (auto v = extract.template operator()<property::Double, double>()) return *v;
  if (auto v = extract.template operator()<property::Float, float>()) return *v;
  return Incompatible{};
}

using Accessor = Value (*)(const Message&);

Value nullableString(const std::string& text, bool explicitEmpty = false) {
  if (text.empty() && !explicitEmpty) return {};
  return std::string_view(text);
}

Accessor header(std::string_view name) {
  if (name == "JMSDeliveryMode") return [](const Message& m) -> Value {
    return std::string_view(m.isPersistent() ? "PERSISTENT" : "NON_PERSISTENT");
  };
  if (name == "JMSPriority") return [](const Message& m) -> Value { return m.jmsHeaders.priority; };
  if (name == "JMSTimestamp") return [](const Message& m) -> Value { return m.jmsHeaders.timestamp; };
  if (name == "JMSMessageID") return [](const Message& m) { return nullableString(m.jmsHeaders.messageId); };
  if (name == "JMSType") return [](const Message& m) { return nullableString(m.jmsHeaders.type, m.jmsHeaders.explicitEmptyType); };
  if (name == "JMSCorrelationID") return [](const Message& m) -> Value {
    if (m.jmsHeaders.binaryCorrelationId) return {};
    return nullableString(m.jmsHeaders.correlationId, m.jmsHeaders.explicitEmptyCorrelationId);
  };
  return nullptr;
}

constexpr int anyCharacter = -2;
constexpr int anySequence = -3;

std::vector<int> compilePattern(const std::string& pattern, int escape) {
  std::vector<int> result;
  for (size_t pos = 0; pos < pattern.size();) {
    int ch = unicodeAt(pattern, pos);
    if (ch < 0) throw InvalidSelectorException("invalid UTF-8 in LIKE pattern");
    if (ch == escape) {
      ch = unicodeAt(pattern, pos);
      if (ch < 0 || (ch != escape && ch != '%' && ch != '_')) {
        throw InvalidSelectorException("invalid LIKE escape sequence");
      }
      result.push_back(ch);
    } else if (ch == '%') {
      if (result.empty() || result.back() != anySequence) result.push_back(anySequence);
    } else {
      result.push_back(ch == '_' ? anyCharacter : ch);
    }
  }
  return result;
}

bool matchPattern(std::string_view value, const std::vector<int>& pattern) {
  size_t pos = 0, part = 0, wildcard = std::string_view::npos, retry = 0;
  while (pos < value.size()) {
    size_t next = pos;
    const auto ch = unicodeAt(value, next);
    if (ch < 0) return false;
    if (part < pattern.size() && (pattern[part] == ch || pattern[part] == anyCharacter)) {
      ++part;
      pos = next;
    } else if (part < pattern.size() && pattern[part] == anySequence) {
      wildcard = part++;
      retry = pos;
    } else if (wildcard != std::string_view::npos) {
      unicodeAt(value, retry);
      pos = retry;
      part = wildcard + 1;
    } else {
      return false;
    }
  }
  return part == pattern.size() || (part + 1 == pattern.size() && pattern.back() == anySequence);
}

Value parseNumber(std::string text, bool negative) {
  const bool hex = text.starts_with("0x") || text.starts_with("0X");
  const bool binary = text.starts_with("0b") || text.starts_with("0B");
  const bool approximate = !binary && (hex ? text.find_first_of(".pP") != std::string::npos
      : text.find_first_of(".eEfFdD") != std::string::npos);
  auto decimalOrHexDigit = [](char c) {
    return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
  };
  // Validate separator placement before removing separators for the library parser.
  for (size_t pos = text.find('_'); pos != std::string::npos;) {
    const size_t next = text.find_first_not_of('_', pos);
    const auto valid = hex && text.find_first_of("pP") > pos ? +decimalOrHexDigit : digit;
    if (pos == 0 || next == std::string::npos || !valid(text[pos - 1]) || !valid(text[next])) {
      throw InvalidSelectorException("invalid separator in numeric literal");
    }
    pos = text.find('_', next);
  }
  text.erase(std::remove(text.begin(), text.end(), '_'), text.end());

  if (approximate) {
    const bool useFloat = text.back() == 'f' || text.back() == 'F';
    if (useFloat || text.back() == 'd' || text.back() == 'D') text.pop_back();
    if (hex && text.find_first_of("pP") == std::string::npos) {
      throw InvalidSelectorException("hexadecimal floating-point literal requires an exponent");
    }
    auto decimal = [&]<class R>() -> Value {
      R number{};
      auto start = text.data() + (hex ? 2 : 0);
      const auto end = text.data() + text.size();
      const auto parsed = std::from_chars(start, end, number, hex ? std::chars_format::hex : std::chars_format::general);
      if (parsed.ec != std::errc{} || parsed.ptr != end) throw InvalidSelectorException("invalid approximate numeric literal");
      return negative ? -number : number;
    };
    return useFloat ? decimal.operator()<float>() : decimal.operator()<double>();
  }

  const bool useLong = text.back() == 'l' || text.back() == 'L';
  if (useLong) text.pop_back();
  const bool octal = text.size() > 1 && text.front() == '0' && !hex && !binary;
  uint64_t number = 0;
  const auto end = text.data() + text.size();
  const auto result = std::from_chars(text.data() + (hex || binary ? 2 : 0), end, number,
                                     hex ? 16 : binary ? 2 : octal ? 8 : 10);
  if (result.ec != std::errc{} || result.ptr != end) throw InvalidSelectorException("invalid integer literal");
  if (hex || binary || octal) {
    if (!useLong && number <= UINT32_MAX) {
      auto bits = static_cast<uint32_t>(number);
      return std::bit_cast<int32_t>(negative ? 0u - bits : bits);
    }
    return std::bit_cast<int64_t>(negative ? uint64_t{0} - number : number);
  }
  const auto max = static_cast<uint64_t>(INT64_MAX);
  if (number > max + negative) throw InvalidSelectorException("integer literal exceeds Java long");
  if (!useLong && number <= static_cast<uint64_t>(INT32_MAX) + negative) {
    auto bits = static_cast<uint32_t>(number);
    return std::bit_cast<int32_t>(negative ? 0u - bits : bits);
  }
  return std::bit_cast<int64_t>(negative ? uint64_t{0} - number : number);
}

class Parser {
 public:
  explicit Parser(const std::string& text) : tokenizer(text), current(tokenizer.next()) {}

  Eval compile() {
    if (current.type == T::End) return [](const Message&) -> Value { return true; };
    auto expression = parseOr();
    requireConditional(expression.kind);
    consume(T::End);
    return std::move(expression.evaluate);
  }

 private:
  enum class ExpressionKind { Identifier, Numeric, String, Conditional };

  struct Expression {
    Eval evaluate;
    ExpressionKind kind;
  };

  Tokenizer tokenizer;
  Token current;
  size_t tokens = 0;

  bool accept(T type) {
    if (current.type != type) return false;
    advance();
    return true;
  }

  Token consume(T type) {
    if (current.type != type) throw InvalidSelectorException("unexpected token '" + current.text + "'");
    Token result = std::move(current);
    advance();
    return result;
  }

  void advance() {
    if (++tokens > 512) throw InvalidSelectorException("selector exceeds 512 tokens");
    current = tokenizer.next();
  }

  static void requireConditional(ExpressionKind kind) {
    if (kind != ExpressionKind::Identifier && kind != ExpressionKind::Conditional) {
      throw InvalidSelectorException("expected a conditional expression");
    }
  }

  static void requireArithmetic(ExpressionKind kind) {
    if (kind != ExpressionKind::Identifier && kind != ExpressionKind::Numeric) {
      throw InvalidSelectorException("expected an arithmetic expression");
    }
  }

  Expression parseOr() { return parseLogical(&Parser::parseAnd, T::Or, true); }
  Expression parseAnd() { return parseLogical(&Parser::parseNot, T::And, false); }

  Expression parseLogical(Expression (Parser::*operand)(), T op, bool shortCircuitValue) {
    auto left = (this->*operand)();
    while (accept(op)) {
      auto right = (this->*operand)();
      requireConditional(left.kind);
      requireConditional(right.kind);
      left = {[l = std::move(left.evaluate), r = std::move(right.evaluate), shortCircuitValue](const Message& m) -> Value {
        auto value = l(m);
        if (auto* b = std::get_if<bool>(&value); b && *b == shortCircuitValue) return shortCircuitValue;
        return logical(value, r(m), shortCircuitValue);
      }, ExpressionKind::Conditional};
    }
    return left;
  }

  Expression parseNot() {
    if (!accept(T::Not)) return parsePredicate();
    auto expr = parseNot();
    requireConditional(expr.kind);
    return {[eval = std::move(expr.evaluate)](const Message& m) { return negate(eval(m)); },
            ExpressionKind::Conditional};
  }

  Expression parsePredicate() {
    auto left = parseSum();
    if (accept(T::Is)) {
      requireIdentifier(left);
      const bool negated = accept(T::Not);
      consume(T::Null);
      return {[eval = std::move(left.evaluate), negated](const Message& m) -> Value {
        return isNull(eval(m)) != negated;
      }, ExpressionKind::Conditional};
    }

    const bool negated = accept(T::Not);
    if (accept(T::Between)) {
      auto low = parseSum();
      consume(T::And);
      auto high = parseSum();
      requireArithmetic(left.kind);
      requireArithmetic(low.kind);
      requireArithmetic(high.kind);
      return {[eval = std::move(left.evaluate), lo = std::move(low.evaluate),
               hi = std::move(high.evaluate), negated](const Message& m) -> Value {
        auto value = eval(m);
        auto lower = compareValues(value, lo(m), negated ? T::Lt : T::Ge);
        auto upper = compareValues(value, hi(m), negated ? T::Gt : T::Le);
        return logical(lower, upper, negated);
      }, ExpressionKind::Conditional};
    }

    if (accept(T::In)) return parseIn(std::move(left), negated);
    if (accept(T::Like)) return parseLike(std::move(left), negated);
    if (negated) throw InvalidSelectorException("expected BETWEEN, IN, or LIKE after NOT");

    switch (current.type) {
      case T::Eq: case T::Ne: case T::Lt: case T::Le: case T::Gt: case T::Ge: {
        const auto op = current.type;
        advance();
        auto right = parseSum();
        return {[l = std::move(left.evaluate), r = std::move(right.evaluate), op](const Message& m) {
          return compareValues(l(m), r(m), op);
        }, ExpressionKind::Conditional};
      }
      default: return left;
    }
  }

  static void requireIdentifier(const Expression& expression) {
    if (expression.kind != ExpressionKind::Identifier) {
      throw InvalidSelectorException("expected a header or property identifier");
    }
  }

  Expression parseIn(Expression left, bool negated) {
    requireIdentifier(left);
    consume(T::Left);
    // Keep the existing C++ numeric IN form as an extension with homogeneous literal operands.
    if (current.type != T::String) {
      std::vector<Value> values;
      do {
        bool negative = accept(T::Minus);
        if (!negative) accept(T::Plus);
        values.push_back(parseNumber(consume(T::Number).text, negative));
      } while (accept(T::Comma));
      consume(T::Right);
      return {[l = std::move(left.evaluate), values = std::move(values), negated](const Message& m) -> Value {
        auto value = l(m);
        if (isNull(value)) return {};
        if (!isNumeric(value)) return false;
        auto matches = [&](const Value& candidate) {
          return truth(compareValues(value, candidate, T::Eq)) == SelectorTruth::True;
        };
        return std::any_of(values.begin(), values.end(), matches) != negated;
      }, ExpressionKind::Conditional};
    }

    std::vector<std::string> values;
    do {
      values.push_back(consume(T::String).text);
    } while (accept(T::Comma));
    consume(T::Right);
    return {[l = std::move(left.evaluate), values = std::move(values), negated](const Message& m) -> Value {
      auto value = l(m);
      if (isNull(value)) return {};
      if (auto* text = std::get_if<std::string_view>(&value)) {
        return (std::find(values.begin(), values.end(), *text) != values.end()) != negated;
      }
      return false;
    }, ExpressionKind::Conditional};
  }

  Expression parseLike(Expression left, bool negated) {
    requireIdentifier(left);
    const auto pattern = consume(T::String).text;
    int escape = -1;
    if (accept(T::Escape)) {
      auto text = consume(T::String).text;
      size_t pos = 0;
      escape = unicodeAt(text, pos);
      if (escape < 0 || pos != text.size()) throw InvalidSelectorException("ESCAPE requires one character");
    }
    return {[l = std::move(left.evaluate), pattern = compilePattern(pattern, escape), negated](const Message& m) -> Value {
      auto value = l(m);
      if (isNull(value)) return {};
      if (auto* text = std::get_if<std::string_view>(&value)) return matchPattern(*text, pattern) != negated;
      return false;
    }, ExpressionKind::Conditional};
  }

  Expression parseSum() { return parseArithmetic(&Parser::parseProduct, T::Plus, T::Minus); }
  Expression parseProduct() { return parseArithmetic(&Parser::parseUnary, T::Multiply, T::Divide); }

  Expression parseArithmetic(Expression (Parser::*operand)(), T first, T second) {
    auto left = (this->*operand)();
    while (current.type == first || current.type == second) {
      const auto op = current.type;
      advance();
      auto right = (this->*operand)();
      requireArithmetic(left.kind);
      requireArithmetic(right.kind);
      left = {[l = std::move(left.evaluate), r = std::move(right.evaluate), op](const Message& m) {
        return calculate(l(m), r(m), op);
      }, ExpressionKind::Numeric};
    }
    return left;
  }

  Expression parseUnary() {
    const auto op = current.type;
    if (op != T::Plus && op != T::Minus) return parsePrimary();
    advance();
    if (current.type == T::Number) return number(op == T::Minus);
    auto expr = parseUnary();
    requireArithmetic(expr.kind);
    return {[eval = std::move(expr.evaluate), op](const Message& m) -> Value {
      auto value = eval(m);
      if (op == T::Minus) return calculate(int32_t{0}, value, op);
      if (isNull(value) || isNumeric(value)) return value;
      return Incompatible{};
    }, ExpressionKind::Numeric};
  }

  Expression number(bool negative) {
    auto value = parseNumber(consume(T::Number).text, negative);
    return {[value](const Message&) { return value; }, ExpressionKind::Numeric};
  }

  Expression parsePrimary() {
    if (accept(T::Left)) {
      auto expr = parseOr();
      consume(T::Right);
      return expr;
    }
    if (accept(T::True)) return {[](const Message&) -> Value { return true; }, ExpressionKind::Conditional};
    if (accept(T::False)) return {[](const Message&) -> Value { return false; }, ExpressionKind::Conditional};
    if (current.type == T::Number) return number(false);
    if (current.type == T::String) {
      return {[text = consume(T::String).text](const Message&) -> Value { return std::string_view(text); },
              ExpressionKind::String};
    }
    auto identifier = consume(T::Identifier).text;
    if (auto accessor = header(identifier)) return {accessor, ExpressionKind::Identifier};
    return {[name = std::move(identifier)](const Message& m) { return propertyValue(m, name); }, ExpressionKind::Identifier};
  }
};

}  // namespace

struct Selector::Impl {
  Eval evaluate;
};

Selector::Selector(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
Selector::~Selector() = default;

std::shared_ptr<Selector> Selector::parse(const std::string& expression) {
  return std::shared_ptr<Selector>(new Selector(std::make_unique<Impl>(Impl{Parser(expression).compile()})));
}

SelectorTruth Selector::evaluate(const Message& message) const {
  return truth(_impl->evaluate(message));
}

bool Selector::matches(const Message& message) const {
  return evaluate(message) == SelectorTruth::True;
}

}  // namespace tiny_mq
