//
// Created by Alexander Bychuk on 01.08.2026.
//
// Regression coverage for tasks/linux-port/01-anyvisitor-dangling-capture.md:
// `Poco::AnyVisitor::insertVisitor` used to capture its `std::function` parameter by
// reference (`[&f]`). That parameter is destroyed when `insertVisitor` returns, so every
// stored visitor held a dangling reference — undefined behaviour that happened to look
// harmless under libc++ (macOS) but reliably corrupted dispatch under libstdc++ (Linux):
// `valueType()` always reported the type of the *last* registered visitor.

#include "AnyVisitorTest.h"
#include "PocoAnyVisitor.h"
#include "MessageProperty.h"
#include <Poco/Any.h>
#include <functional>
#include <string>

namespace {
// Registers visitors for several distinct types into `visitor`. The std::function objects
// passed to insertVisitor are ordinary parameters of *this* function; they are destroyed
// the moment it returns. Correct behaviour requires insertVisitor to keep its own copy —
// with the old `[&f]` capture, the caller would be left holding dangling references.
void registerVisitors(Poco::AnyVisitor& visitor, std::string& intTag, std::string& stringTag, std::string& doubleTag) {
  std::function<void(const int&)> intVisitor = [&intTag](const int&) { intTag = "int"; };
  std::function<void(const std::string&)> stringVisitor = [&stringTag](const std::string&) { stringTag = "string"; };
  std::function<void(const double&)> doubleVisitor = [&doubleTag](const double&) { doubleTag = "double"; };
  visitor.insertVisitor<int>(intVisitor);
  visitor.insertVisitor<std::string>(stringVisitor);
  visitor.insertVisitor<double>(doubleVisitor);
}

// Deliberately reuses the stack memory that registerVisitors' frame occupied, so a dangling
// reference (if one existed) would observe clobbered garbage rather than "coincidentally"
// intact data. This is what makes the test a reliable UB detector rather than a lucky pass.
__attribute__((noinline)) void clobberStack() {
  volatile char noise[1024];
  for (auto& c : noise) c = static_cast<char>(0x5A);
}
}  // namespace

TEST_F(AnyVisitorTest, invokesVisitorForActualTypeAfterRegistrationScopeEnds) {
  Poco::AnyVisitor visitor;
  std::string intTag, stringTag, doubleTag;

  registerVisitors(visitor, intTag, stringTag, doubleTag);
  clobberStack();

  Poco::Any stringValue(std::string("hello"));
  Poco::Any intValue(42);
  Poco::Any doubleValue(3.14);

  ASSERT_TRUE(visitor(stringValue));
  EXPECT_EQ(stringTag, "string");
  EXPECT_TRUE(intTag.empty());
  EXPECT_TRUE(doubleTag.empty());

  ASSERT_TRUE(visitor(intValue));
  EXPECT_EQ(intTag, "int");

  ASSERT_TRUE(visitor(doubleValue));
  EXPECT_EQ(doubleTag, "double");
}

TEST_F(AnyVisitorTest, propertyValueTypeReturnsActualTypeForEachPropertyKind) {
  using namespace tiny_mq::property;
  tiny_mq::Properties props;
  props.setProperty("bool", Bool(true));
  props.setProperty("byte", Byte(static_cast<raw_type::byte>(1)));
  props.setProperty("char", Char('c'));
  props.setProperty("short", Short(static_cast<raw_type::short_integer>(2)));
  props.setProperty("int", Int(3));
  props.setProperty("long", Long(static_cast<raw_type::long_integer>(4)));
  props.setProperty("float", Float(1.5f));
  props.setProperty("double", Double(2.5));
  props.setProperty("string", String(std::string("s")));
  props.setProperty("bytes", Bytes(tiny_mq::BytesVector{1, 2, 3}));

  // Each lookup must report the type of *its own* value, not BYTE_ARRAY_TYPE (10) — the
  // type of the last-registered visitor in Properties::propertyValueType, which is what the
  // dangling-reference bug always returned regardless of the actual stored type.
  EXPECT_EQ(props.propertyValueType("bool"), BOOLEAN_TYPE);
  EXPECT_EQ(props.propertyValueType("byte"), BYTE_TYPE);
  EXPECT_EQ(props.propertyValueType("char"), CHAR_TYPE);
  EXPECT_EQ(props.propertyValueType("short"), SHORT_TYPE);
  EXPECT_EQ(props.propertyValueType("int"), INTEGER_TYPE);
  EXPECT_EQ(props.propertyValueType("long"), LONG_TYPE);
  EXPECT_EQ(props.propertyValueType("float"), FLOAT_TYPE);
  EXPECT_EQ(props.propertyValueType("double"), DOUBLE_TYPE);
  EXPECT_EQ(props.propertyValueType("string"), STRING_TYPE);
  EXPECT_EQ(props.propertyValueType("bytes"), BYTE_ARRAY_TYPE);
}
