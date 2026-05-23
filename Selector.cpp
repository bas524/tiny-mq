//
// Created by Alexander Bychuk on 12.04.2026.
//

#include "Selector.h"
#include "Message.h"
#include "MessageProperty.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <regex>
#include <vector>

namespace tiny_mq {

// ============================================================
// Tokenizer
// ============================================================

enum class TokenType {
  IDENT,
  STRING,
  INT_LIT,
  FLOAT_LIT,
  TRUE_LIT,
  FALSE_LIT,
  NULL_LIT,
  AND,
  OR,
  NOT,
  LIKE,
  BETWEEN,
  IN,
  IS,
  EQ,   // =
  NEQ,  // <>
  LT,   // <
  LTE,  // <=
  GT,   // >
  GTE,  // >=
  LPAREN,
  RPAREN,
  COMMA,
  EOF_TOKEN
};

struct Token {
  TokenType type;
  std::string text;
};

class Tokenizer {
 public:
  explicit Tokenizer(const std::string &input) : _input(input), _pos(0) {}

  Token next() {
    skipWhitespace();
    if (_pos >= _input.size()) return {TokenType::EOF_TOKEN, ""};

    char c = _input[_pos];

    if (c == '\'') return readString();

    if (std::isdigit(static_cast<unsigned char>(c)) ||
        (c == '-' && _pos + 1 < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_pos + 1])))) {
      return readNumber();
    }

    if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') return readIdent();

    switch (c) {
      case '=': _pos++; return {TokenType::EQ, "="};
      case '(': _pos++; return {TokenType::LPAREN, "("};
      case ')': _pos++; return {TokenType::RPAREN, ")"};
      case ',': _pos++; return {TokenType::COMMA, ","};
      case '<':
        _pos++;
        if (_pos < _input.size() && _input[_pos] == '>') { _pos++; return {TokenType::NEQ, "<>"}; }
        if (_pos < _input.size() && _input[_pos] == '=') { _pos++; return {TokenType::LTE, "<="}; }
        return {TokenType::LT, "<"};
      case '>':
        _pos++;
        if (_pos < _input.size() && _input[_pos] == '=') { _pos++; return {TokenType::GTE, ">="}; }
        return {TokenType::GT, ">"};
      default: break;
    }
    throw InvalidSelectorException(std::string("unexpected character '") + c + "'");
  }

 private:
  const std::string &_input;
  size_t _pos;

  void skipWhitespace() {
    while (_pos < _input.size() && std::isspace(static_cast<unsigned char>(_input[_pos]))) ++_pos;
  }

  Token readString() {
    ++_pos;  // skip opening quote
    std::string result;
    while (_pos < _input.size()) {
      char ch = _input[_pos++];
      if (ch == '\'') {
        if (_pos < _input.size() && _input[_pos] == '\'') {
          result += '\'';
          ++_pos;
        } else {
          break;  // end of string literal
        }
      } else {
        result += ch;
      }
    }
    return {TokenType::STRING, result};
  }

  Token readNumber() {
    size_t start = _pos;
    bool isFloat = false;
    if (_input[_pos] == '-') ++_pos;
    while (_pos < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_pos]))) ++_pos;
    if (_pos < _input.size() && _input[_pos] == '.') {
      isFloat = true;
      ++_pos;
      while (_pos < _input.size() && std::isdigit(static_cast<unsigned char>(_input[_pos]))) ++_pos;
    }
    return {isFloat ? TokenType::FLOAT_LIT : TokenType::INT_LIT, _input.substr(start, _pos - start)};
  }

  Token readIdent() {
    size_t start = _pos;
    while (_pos < _input.size() &&
           (std::isalnum(static_cast<unsigned char>(_input[_pos])) || _input[_pos] == '_' || _input[_pos] == '.')) {
      ++_pos;
    }
    std::string text = _input.substr(start, _pos - start);
    std::string upper = text;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char ch) { return std::toupper(ch); });

    if (upper == "AND") return {TokenType::AND, text};
    if (upper == "OR") return {TokenType::OR, text};
    if (upper == "NOT") return {TokenType::NOT, text};
    if (upper == "LIKE") return {TokenType::LIKE, text};
    if (upper == "BETWEEN") return {TokenType::BETWEEN, text};
    if (upper == "IN") return {TokenType::IN, text};
    if (upper == "IS") return {TokenType::IS, text};
    if (upper == "TRUE") return {TokenType::TRUE_LIT, text};
    if (upper == "FALSE") return {TokenType::FALSE_LIT, text};
    if (upper == "NULL") return {TokenType::NULL_LIT, text};

    return {TokenType::IDENT, text};
  }
};

// ============================================================
// Evaluator: expressions as std::function closures
// ============================================================

using EvalFn = std::function<SelectorValue(const Message &)>;

// ---- Property extraction ----

static SelectorValue extractProperty(const Message &msg, const std::string &name) {
  if (!msg.hasProperty(name)) return std::monostate{};
  switch (msg.propertyValueType(name)) {
    case property::BOOLEAN_TYPE: {
      const auto &v = msg.property<property::Bool>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{v.value()};
    }
    case property::STRING_TYPE: {
      const auto &v = msg.property<property::String>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{v.value()};
    }
    case property::INTEGER_TYPE: {
      const auto &v = msg.property<property::Int>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{static_cast<int64_t>(v.value())};
    }
    case property::LONG_TYPE: {
      const auto &v = msg.property<property::Long>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{static_cast<int64_t>(v.value())};
    }
    case property::SHORT_TYPE: {
      const auto &v = msg.property<property::Short>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{static_cast<int64_t>(v.value())};
    }
    case property::BYTE_TYPE: {
      const auto &v = msg.property<property::Byte>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{static_cast<int64_t>(v.value())};
    }
    case property::DOUBLE_TYPE: {
      const auto &v = msg.property<property::Double>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{static_cast<double>(v.value())};
    }
    case property::FLOAT_TYPE: {
      const auto &v = msg.property<property::Float>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{static_cast<double>(v.value())};
    }
    case property::CHAR_TYPE: {
      const auto &v = msg.property<property::Char>(name);
      return v.isNull() ? SelectorValue{std::monostate{}} : SelectorValue{std::string(1, v.value())};
    }
    default: return std::monostate{};
  }
}

// ---- Value comparison helpers ----

static double toDouble(const SelectorValue &v) {
  if (std::holds_alternative<int64_t>(v)) return static_cast<double>(std::get<int64_t>(v));
  return std::get<double>(v);
}

static bool isNumeric(const SelectorValue &v) {
  return std::holds_alternative<int64_t>(v) || std::holds_alternative<double>(v);
}

static bool selEQ(const SelectorValue &a, const SelectorValue &b) {
  if (std::holds_alternative<std::monostate>(a) || std::holds_alternative<std::monostate>(b)) return false;
  if (isNumeric(a) && isNumeric(b)) return toDouble(a) == toDouble(b);
  if (a.index() != b.index()) return false;
  return a == b;
}

static bool selLT(const SelectorValue &a, const SelectorValue &b) {
  if (!isNumeric(a) || !isNumeric(b)) return false;
  return toDouble(a) < toDouble(b);
}

static bool selGT(const SelectorValue &a, const SelectorValue &b) { return selLT(b, a); }
static bool selLTE(const SelectorValue &a, const SelectorValue &b) { return selEQ(a, b) || selLT(a, b); }
static bool selGTE(const SelectorValue &a, const SelectorValue &b) { return selEQ(a, b) || selGT(a, b); }

// ---- LIKE pattern → std::regex ----

static std::string likeToRegex(const std::string &pattern) {
  std::string result;
  result.reserve(pattern.size() * 2 + 2);
  result += '^';
  for (char ch : pattern) {
    switch (ch) {
      case '%': result += ".*"; break;
      case '_': result += '.'; break;
      case '.': case '^': case '$': case '*': case '+':
      case '?': case '(': case ')': case '[': case ']':
      case '{': case '}': case '|': case '\\':
        result += '\\';
        result += ch;
        break;
      default: result += ch; break;
    }
  }
  result += '$';
  return result;
}

// ============================================================
// Recursive-descent parser
// ============================================================

class Parser {
 public:
  explicit Parser(const std::string &input) : _tok(input) { advance(); }

  EvalFn parseExpression() {
    EvalFn expr = parseOr();
    if (_current.type != TokenType::EOF_TOKEN) {
      throw InvalidSelectorException("unexpected token '" + _current.text + "'");
    }
    return expr;
  }

 private:
  Tokenizer _tok;
  Token _current{TokenType::EOF_TOKEN, ""};

  void advance() { _current = _tok.next(); }

  bool check(TokenType t) const { return _current.type == t; }

  Token consume(TokenType t, const std::string &what) {
    if (_current.type != t) throw InvalidSelectorException("expected " + what + ", got '" + _current.text + "'");
    Token tok = _current;
    advance();
    return tok;
  }

  // ---- Grammar ----
  // expression  := or_expr
  // or_expr     := and_expr (OR and_expr)*
  // and_expr    := not_expr (AND not_expr)*
  // not_expr    := NOT not_expr | comparison
  // comparison  := primary (IS [NOT] NULL |
  //                         [NOT] BETWEEN primary AND primary |
  //                         [NOT] IN '(' primary (',' primary)* ')' |
  //                         [NOT] LIKE STRING |
  //                         (= | <> | < | <= | > | >=) primary)?
  // primary     := '(' expression ')' | NULL | TRUE | FALSE | STRING | INT | FLOAT | IDENT

  EvalFn parseOr() {
    auto left = parseAnd();
    while (check(TokenType::OR)) {
      advance();
      auto right = parseAnd();
      left = [l = left, r = right](const Message &msg) -> SelectorValue {
        auto lv = l(msg);
        if (std::holds_alternative<bool>(lv) && std::get<bool>(lv)) return true;
        auto rv = r(msg);
        if (std::holds_alternative<bool>(rv) && std::get<bool>(rv)) return true;
        if (std::holds_alternative<bool>(lv) && std::holds_alternative<bool>(rv)) return false;
        return std::monostate{};
      };
    }
    return left;
  }

  EvalFn parseAnd() {
    auto left = parseNot();
    while (check(TokenType::AND)) {
      advance();
      auto right = parseNot();
      left = [l = left, r = right](const Message &msg) -> SelectorValue {
        auto lv = l(msg);
        if (std::holds_alternative<bool>(lv) && !std::get<bool>(lv)) return false;
        auto rv = r(msg);
        if (std::holds_alternative<bool>(rv) && !std::get<bool>(rv)) return false;
        if (std::holds_alternative<bool>(lv) && std::holds_alternative<bool>(rv)) return true;
        return std::monostate{};
      };
    }
    return left;
  }

  EvalFn parseNot() {
    if (check(TokenType::NOT)) {
      advance();
      auto expr = parseNot();
      return [e = expr](const Message &msg) -> SelectorValue {
        auto v = e(msg);
        if (std::holds_alternative<bool>(v)) return !std::get<bool>(v);
        return std::monostate{};
      };
    }
    return parseComparison();
  }

  EvalFn parseComparison() {
    auto left = parsePrimary();

    // IS [NOT] NULL
    if (check(TokenType::IS)) {
      advance();
      bool negated = false;
      if (check(TokenType::NOT)) { advance(); negated = true; }
      consume(TokenType::NULL_LIT, "NULL");
      return [l = left, negated](const Message &msg) -> SelectorValue {
        auto v = l(msg);
        bool isNull = std::holds_alternative<std::monostate>(v);
        return negated ? !isNull : isNull;
      };
    }

    // Optional NOT before BETWEEN / IN / LIKE
    bool negated = false;
    if (check(TokenType::NOT)) {
      advance();
      negated = true;
    }

    if (check(TokenType::BETWEEN)) {
      advance();
      auto low = parsePrimary();
      consume(TokenType::AND, "AND");
      auto high = parsePrimary();
      return [l = left, lo = low, hi = high, negated](const Message &msg) -> SelectorValue {
        auto lv = l(msg);
        auto lov = lo(msg);
        auto hiv = hi(msg);
        bool result = selGTE(lv, lov) && selLTE(lv, hiv);
        return negated ? !result : result;
      };
    }

    if (check(TokenType::IN)) {
      advance();
      consume(TokenType::LPAREN, "(");
      std::vector<EvalFn> items;
      items.push_back(parsePrimary());
      while (check(TokenType::COMMA)) {
        advance();
        items.push_back(parsePrimary());
      }
      consume(TokenType::RPAREN, ")");
      return [l = left, items = std::move(items), negated](const Message &msg) -> SelectorValue {
        auto lv = l(msg);
        if (std::holds_alternative<std::monostate>(lv)) return std::monostate{};
        for (const auto &item : items) {
          if (selEQ(lv, item(msg))) return !negated;
        }
        return negated;
      };
    }

    if (check(TokenType::LIKE)) {
      advance();
      Token patTok = consume(TokenType::STRING, "pattern string");
      std::string regex = likeToRegex(patTok.text);
      return [l = left, regex = std::move(regex), negated](const Message &msg) -> SelectorValue {
        auto lv = l(msg);
        if (!std::holds_alternative<std::string>(lv)) return std::monostate{};
        bool result = std::regex_match(std::get<std::string>(lv), std::regex(regex));
        return negated ? !result : result;
      };
    }

    if (negated) {
      throw InvalidSelectorException("NOT must be followed by BETWEEN, IN, or LIKE");
    }

    // Relational operators
    if (check(TokenType::EQ) || check(TokenType::NEQ) || check(TokenType::LT) || check(TokenType::LTE) ||
        check(TokenType::GT) || check(TokenType::GTE)) {
      TokenType op = _current.type;
      advance();
      auto right = parsePrimary();
      return [l = left, r = right, op](const Message &msg) -> SelectorValue {
        auto lv = l(msg);
        auto rv = r(msg);
        switch (op) {
          case TokenType::EQ:  return selEQ(lv, rv);
          case TokenType::NEQ: return !selEQ(lv, rv);
          case TokenType::LT:  return selLT(lv, rv);
          case TokenType::LTE: return selLTE(lv, rv);
          case TokenType::GT:  return selGT(lv, rv);
          case TokenType::GTE: return selGTE(lv, rv);
          default:             return std::monostate{};
        }
      };
    }

    return left;
  }

  EvalFn parsePrimary() {
    if (check(TokenType::LPAREN)) {
      advance();
      EvalFn expr = parseOr();
      consume(TokenType::RPAREN, ")");
      return expr;
    }

    if (check(TokenType::NULL_LIT)) {
      advance();
      return [](const Message &) -> SelectorValue { return std::monostate{}; };
    }

    if (check(TokenType::TRUE_LIT)) {
      advance();
      return [](const Message &) -> SelectorValue { return true; };
    }

    if (check(TokenType::FALSE_LIT)) {
      advance();
      return [](const Message &) -> SelectorValue { return false; };
    }

    if (check(TokenType::STRING)) {
      std::string val = _current.text;
      advance();
      return [val](const Message &) -> SelectorValue { return val; };
    }

    if (check(TokenType::INT_LIT)) {
      int64_t val = std::stoll(_current.text);
      advance();
      return [val](const Message &) -> SelectorValue { return val; };
    }

    if (check(TokenType::FLOAT_LIT)) {
      double val = std::stod(_current.text);
      advance();
      return [val](const Message &) -> SelectorValue { return val; };
    }

    if (check(TokenType::IDENT)) {
      std::string name = _current.text;
      advance();
      return [name](const Message &msg) -> SelectorValue { return extractProperty(msg, name); };
    }

    throw InvalidSelectorException("unexpected token '" + _current.text + "' in expression");
  }
};

// ============================================================
// Selector implementation
// ============================================================

struct Selector::Impl {
  EvalFn evalFn;
};

Selector::Selector(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
Selector::~Selector() = default;

std::shared_ptr<Selector> Selector::parse(const std::string &expression) {
  auto impl = std::make_unique<Impl>();
  if (expression.empty()) {
    impl->evalFn = [](const Message &) -> SelectorValue { return true; };
  } else {
    Parser parser(expression);
    impl->evalFn = parser.parseExpression();
  }
  return std::shared_ptr<Selector>(new Selector(std::move(impl)));
}

bool Selector::matches(const Message &message) const {
  auto v = _impl->evalFn(message);
  return std::holds_alternative<bool>(v) && std::get<bool>(v);
}

}  // namespace tiny_mq
