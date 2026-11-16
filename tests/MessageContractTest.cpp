#include "BytesMessage.h"
#include "Connection.h"
#include "Exchange.h"
#include "MapMessage.h"
#include "Message.h"
#include "ObjectMessage.h"
#include "Selector.h"
#include "Session.h"
#include "StreamMessage.h"
#include "TextMessage.h"
#include "TestHelper.h"

#include <Poco/Timestamp.h>
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <utility>

namespace {
using namespace tiny_mq;

class MessageContractTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ifstream input(TINY_MQ_MESSAGE_CONTRACT_FIXTURE);
    ASSERT_TRUE(input.good());
    for (std::string line; std::getline(input, line);) {
      if (line.empty() || line.front() == '#') continue;
      const auto split = line.find('\t');
      const auto value = line.find('\t', split + 1);
      ASSERT_NE(value, std::string::npos);
      records.emplace(line.substr(0, split), std::pair{line.substr(split + 1, value - split - 1),
                                                     line.substr(value + 1)});
    }
  }

  const std::string& field(const std::string& name, const std::string& type) {
    const auto& [actualType, value] = records.at(name);
    EXPECT_EQ(actualType, type);
    return value;
  }

  int32_t integer(const std::string& name) { return std::stoi(field(name, "int")); }
  int64_t integer64(const std::string& name) { return std::stoll(field(name, "long")); }

  BytesVector bytes(const std::string& name) {
    const auto& hex = field(name, "bytes");
    BytesVector data;
    for (size_t offset = 0; offset < hex.size(); offset += 2) {
      data.push_back(static_cast<int8_t>(std::stoi(hex.substr(offset, 2), nullptr, 16)));
    }
    return data;
  }

  std::map<std::string, std::pair<std::string, std::string>> records;
};

int64_t nowMillis() { return Poco::Timestamp().epochMicroseconds() / 1000; }

// REQ-70-1
TEST_F(MessageContractTest, CrossLanguageEnvelope) {
  std::cout << "REQ-70-1\n";
  TextMessage out({}, field("text", "string"));
  out.jmsHeaders.replyTo = field("replyTo", "uri");
  out.jmsHeaders.setCorrelationId(field("correlation", "string"));
  out.jmsHeaders.setType(field("messageType", "string"));
  std::string previousId;
  for (unsigned hints : {0u, 1u, 2u, 3u, 0u}) {
    SCOPED_TRACE(hints);
    out.jmsHeaders.messageId = "client-id";
    out.jmsHeaders.timestamp = -1;
    out.jmsHeaders.priority = 1;
    out.jmsHeaders.destination = "wrong";
    out.jmsHeaders.expiration = -1;
    out.jmsHeaders.deliveryTime = -1;
    out.jmsHeaders.redelivered = true;
    out.jmsHeaders.deliveryCount = 9;
    const auto begin = nowMillis();
    out.prepareForJakartaSend(field("destination", "uri"), Message::PERSISTENT, integer("priority"),
                              integer64("expiration"), integer64("deliveryTime"), hints & 1, hints & 2);
    const auto end = nowMillis();
    TextMessage in;
    in.fromBytes(out.toBytes());
    const auto& headers = in.jmsHeaders;
    EXPECT_EQ(headers.destination, field("destination", "uri"));
    EXPECT_EQ(headers.replyTo, field("replyTo", "uri"));
    EXPECT_EQ(headers.getCorrelationId(), CorrelationId(field("correlation", "string")));
    EXPECT_EQ(headers.type, field("messageType", "string"));
    EXPECT_EQ(headers.priority, integer("priority"));
    EXPECT_EQ(headers.expiration, integer64("expiration"));
    EXPECT_EQ(headers.deliveryTime, integer64("deliveryTime"));
    EXPECT_EQ(in.reliability, Message::PERSISTENT);
    EXPECT_FALSE(headers.redelivered);
    EXPECT_EQ(headers.jmsDeliveryCount(), integer("firstDeliveryCount"));
    if (hints & 1) {
      EXPECT_TRUE(headers.messageId.empty());
    } else {
      EXPECT_TRUE(headers.messageId.starts_with(field("messageIdPrefix", "string")));
      EXPECT_NE(headers.messageId, previousId);
      previousId = headers.messageId;
    }
    if (hints & 2) {
      EXPECT_EQ(headers.timestamp, 0);
    } else {
      EXPECT_GE(headers.timestamp, begin);
      EXPECT_LE(headers.timestamp, end);
    }
    out.jmsHeaders.redelivered = true;
    out.jmsHeaders.deliveryCount = 1;
    in.fromBytes(out.toBytes());
    EXPECT_TRUE(in.jmsHeaders.redelivered);
    EXPECT_EQ(in.jmsHeaders.jmsDeliveryCount(), integer("repeatDeliveryCount"));
  }
}

// REQ-70-1, REQ-70-9
TEST_F(MessageContractTest, V03DeliveryAndTransaction) {
  std::cout << "REQ-70-1, REQ-70-9\n";
  const auto path = CurrentTestSuiteStorageDir() + "-" + CurrentTestName;
  RemoveTestStorageDir(path);
  {
    Exchange exchange(path);
    Connection connection(exchange);
    connection.start();
    for (auto mode : {Session::AUTO_ACKNOWLEDGE, Session::SESSION_TRANSACTED}) {
      auto& session = connection.createSession(mode);
      auto queue = session.createDestination(destination::Queue, std::to_string(mode));
      auto producer = session.createProducer(queue);
      auto consumer = session.createConsumer(queue);
      for (auto persistence : {Message::NOT_PERSISTENT, Message::PERSISTENT}) {
        TextMessage outbound = session.createTextMessage(field("text", "string"), persistence);
        outbound.jmsHeaders.destination = queue->uri();
        outbound.jmsHeaders.setCorrelationId(bytes("binaryCorrelation"));
        SendOptions options{persistence, integer("priority"), 30000, 5};
        producer->send(outbound, options);
        if (mode == Session::SESSION_TRANSACTED) session.commit();
        auto received = consumer->recv(500000);
        ASSERT_NE(received, nullptr);
        EXPECT_EQ(received->jmsHeaders.priority, integer("priority"));
        EXPECT_EQ(received->jmsHeaders.destination, queue->uri());
        EXPECT_EQ(received->jmsHeaders.getCorrelationId(), CorrelationId(bytes("binaryCorrelation")));
        EXPECT_GT(received->jmsHeaders.deliveryTime, 0);
        EXPECT_GT(received->jmsHeaders.expiration, received->jmsHeaders.deliveryTime);
        if (mode == Session::SESSION_TRANSACTED) session.commit();
      }
    }
  }
  RemoveTestStorageDir(path);
}

// REQ-70-2
TEST_F(MessageContractTest, BodyRoundTrip) {
  std::cout << "REQ-70-2\n";
  Message plain;
  Message restoredPlain;
  restoredPlain.fromBytes(plain.toBytes());
  EXPECT_EQ(restoredPlain.type(), Message::UNDEFINED);
  EXPECT_TRUE(restoredPlain.dataAsBytes().empty());

  TextMessage text({}, field("text", "string")), restoredText;
  restoredText.fromBytes(text.toBytes());
  EXPECT_EQ(restoredText.text(), field("text", "string"));
  EXPECT_FALSE(restoredText.isTextNull());
  text.clearData();
  restoredText.fromBytes(text.toBytes());
  EXPECT_TRUE(restoredText.isTextNull());
  text.setText("");
  restoredText.fromBytes(text.toBytes());
  EXPECT_FALSE(restoredText.isTextNull());
  EXPECT_TRUE(restoredText.text().empty());

  BytesMessage binary({}, bytes("binaryCorrelation")), restoredBinary;
  restoredBinary.fromBytes(binary.toBytes());
  EXPECT_EQ(restoredBinary.bytes(), bytes("binaryCorrelation"));

  MapMessage map, restoredMap;
  map.set("empty", property::Bytes(BytesVector{}));
  map.set("n", property::Int(12));
  map.set("nil", property::String{});
  map.set("char", property::JavaChar(char16_t{0x5927}));
  restoredMap.fromBytes(map.toBytes());
  EXPECT_FALSE(restoredMap.get<property::Bytes>("empty").isNull());
  EXPECT_TRUE(restoredMap.get<property::Bytes>("empty").value().empty());
  EXPECT_EQ(restoredMap.get<property::Int>("n").value(), 12);
  EXPECT_TRUE(restoredMap.get<property::String>("nil").isNull());
  EXPECT_EQ(restoredMap.get<property::JavaChar>("char").value(), char16_t{0x5927});
  EXPECT_EQ(map.toBytes().front(), 0x03);

  StreamMessage stream, restoredStream;
  stream.write(int16_t{-12});
  stream.write(BytesVector{});
  stream.writeNull();
  stream.write(char16_t{0xD800});
  stream.reset();
  EXPECT_EQ(stream.copy()->dataAsBytes(), stream.dataAsBytes());
  restoredStream.fromBytes(stream.toBytes());
  restoredStream.reset();
  EXPECT_EQ(restoredStream.read<int16_t>(), -12);
  EXPECT_TRUE(restoredStream.read<BytesVector>().empty());
  EXPECT_EQ(restoredStream.nextValueType(), property::NULL_TYPE);
  restoredStream.readNull();
  EXPECT_EQ(restoredStream.read<char16_t>(), char16_t{0xD800});
  EXPECT_TRUE(restoredStream.eof());
  EXPECT_EQ(stream.toBytes().front(), 0x03);

  for (const std::string name : {"", "com.example.Type"}) {
    ObjectMessage object({}, bytes("binaryCorrelation"), name), restoredObject;
    restoredObject.fromBytes(object.toBytes());
    EXPECT_EQ(restoredObject.className(), name);
    EXPECT_EQ(restoredObject.body(), bytes("binaryCorrelation"));
    EXPECT_TRUE(restoredObject.hasSerializedBody());
    EXPECT_TRUE(Selector::parse("className IS NULL")->matches(restoredObject));
  }
  ObjectMessage empty({}, BytesVector{}, ""), detached;
  detached.fromBytes(empty.toBytes());
  EXPECT_TRUE(detached.hasSerializedBody());
  EXPECT_TRUE(detached.body().empty());
  empty.clearData();
  detached.fromBytes(empty.toBytes());
  EXPECT_FALSE(detached.hasSerializedBody());

  text.setText("before delivery");
  text.setStringProperty("tag", "fixed");
  const auto snapshot = text.toBytes();
  restoredText.fromBytes(snapshot);
  restoredText.setText("edited by recipient");
  restoredText.setStringProperty("tag", "edited");
  restoredText.fromBytes(snapshot);
  EXPECT_EQ(restoredText.text(), "before delivery");
  EXPECT_EQ(restoredText.property<property::String>("tag").value(), "fixed");
}

// REQ-70-4
TEST_F(MessageContractTest, PropertyAccessMatrix) {
  std::cout << "REQ-70-4\n";
  const auto& nullName = field("nullProperty", "name");
  const auto& absentName = field("missing", "name");
  TextMessage source, target;
  source.setNullProperty(nullName);
  source.setIntProperty("$π1", 12);
  source.setIntProperty("$π1", 42);
  source.setFloatProperty("P", -0.0f);
  source.setDoubleProperty("N", std::numeric_limits<double>::quiet_NaN());
  target.fromBytes(source.toBytes());
  EXPECT_TRUE(target.hasProperty(nullName));
  EXPECT_TRUE(target.property<property::String>(nullName).isNull());
  EXPECT_FALSE(target.hasProperty(absentName));
  EXPECT_TRUE(Selector::parse(nullName + " IS NULL AND " + absentName + " IS NULL")->matches(target));
  EXPECT_TRUE(Selector::parse("$π1 = 42 AND P = 0 AND N <> N")->matches(target));
  EXPECT_TRUE(Selector::parse("$Π1 IS NULL AND $π1 IS NOT NULL")->matches(target));
  EXPECT_TRUE(std::signbit(target.property<property::Float>("P").value()));
}

// REQ-70-5
TEST_F(MessageContractTest, SelectorGrammarMatrix) {
  std::cout << "REQ-70-5\n";
  TextMessage message({}, "body");
  message.setStringProperty("p", "2");
  message.setIntProperty("n", 10);
  message.setLongProperty("exact", 9007199254740993LL);
  message.setFloatProperty("apx", 16777216.0f);
  message.setDoubleProperty("nan", std::numeric_limits<double>::quiet_NaN());
  message.setStringProperty("label", "🐈_50%");
  message.jmsHeaders.priority = 7;
  message.jmsHeaders.timestamp = 42;
  message.jmsHeaders.messageId = "ID:fixture";
  message.jmsHeaders.setCorrelationId(std::string("correlation"));
  message.jmsHeaders.setType(std::string("Event"));
  using Truth = SelectorTruth;
  const std::pair<const char*, Truth> cases[] = {
      {"", Truth::True}, {" \t\r\n", Truth::True}, {"n + 2 * (3 - -1) = 18 AND n / +2 = 5", Truth::True},
      {"n = 0xa AND n = 0b1010 AND n = 012", Truth::True},
      {"n = 1.0e1 AND n = 0x1.4p3 AND n = 1_0L", Truth::True},
      {"n = .1e2 AND n = 10. AND n = 10f", Truth::True},
      {"-9223372036854775808L / -1 = -9223372036854775808", Truth::True},
      {"exact = 9007199254740993 AND exact <> 9007199254740992", Truth::True},
      {"exact = 9007199254740992.0 AND apx = 16777217", Truth::True},
      {"FALSE OR TRUE AND FALSE", Truth::False}, {"missing AND FALSE", Truth::False},
      {"missing OR TRUE", Truth::True}, {"NOT missing", Truth::Unknown},
      {"missing OR FALSE", Truth::Unknown}, {"TRUE AND missing", Truth::Unknown},
      {"missing = 1", Truth::Unknown}, {"NOT (missing <> 1)", Truth::Unknown},
      {"n NOT BETWEEN missing AND 5", Truth::True}, {"n BETWEEN missing AND 20", Truth::Unknown},
      {"n BETWEEN 5 AND 20", Truth::True}, {"missing NOT IN ('1')", Truth::Unknown},
      {"p NOT BETWEEN 1 AND 3", Truth::False}, {"nan NOT BETWEEN 1 AND 3", Truth::False}, {"n NOT BETWEEN p AND 20", Truth::False},
      {"n NOT BETWEEN p AND 5", Truth::True}, {"n NOT BETWEEN p AND missing", Truth::Unknown},
      {"p IN ('1', '2') AND p NOT IN ('1', '3')", Truth::True},
      {"label LiKe '_!_%!%' ESCAPE '!'", Truth::True},
      {"label NOT LIKE '🐈X%'", Truth::True}, {"missing NOT LIKE '_%'", Truth::Unknown},
      {"p > 1", Truth::False}, {"p <> 1", Truth::False}, {"p + 1 = 3", Truth::False},
      {"p >= '2'", Truth::False}, {"TRUE < TRUE", Truth::False},
      {"n / 0 = 0", Truth::Unknown}, {"messageBody IS NULL AND JMSDestination IS NULL", Truth::True},
      {"JMSPriority = 7 AND JMSTimestamp = 42 AND JMSMessageID = 'ID:fixture' "
       "AND JMSCorrelationID = 'correlation' AND JMSType = 'Event'", Truth::True}};
  for (const auto& [expression, expected] : cases) {
    SCOPED_TRACE(expression);
    const auto selector = Selector::parse(expression);
    EXPECT_EQ(selector->evaluate(message), expected);
    EXPECT_EQ(selector->matches(message), expected == Truth::True);
  }
  const std::array<std::string, 3> operands{"TRUE", "FALSE", "missing"};
  const Truth andTable[3][3] = {
      {Truth::True, Truth::False, Truth::Unknown},
      {Truth::False, Truth::False, Truth::False},
      {Truth::Unknown, Truth::False, Truth::Unknown}};
  const Truth orTable[3][3] = {
      {Truth::True, Truth::True, Truth::True},
      {Truth::True, Truth::False, Truth::Unknown},
      {Truth::True, Truth::Unknown, Truth::Unknown}};
  const Truth notTable[3] = {Truth::False, Truth::True, Truth::Unknown};
  for (size_t left = 0; left < operands.size(); ++left) {
    EXPECT_EQ(Selector::parse("NOT " + operands[left])->evaluate(message), notTable[left]);
    for (size_t right = 0; right < operands.size(); ++right) {
      EXPECT_EQ(Selector::parse(operands[left] + " AND " + operands[right])->evaluate(message),
                andTable[left][right]);
      EXPECT_EQ(Selector::parse(operands[left] + " OR " + operands[right])->evaluate(message),
                orTable[left][right]);
    }
  }
  EXPECT_TRUE(Selector::parse("JMSDeliveryMode = 'NON_PERSISTENT'")->matches(message));
  message.reliability = Message::PERSISTENT;
  EXPECT_TRUE(Selector::parse("JMSDeliveryMode = 'PERSISTENT'")->matches(message));
  for (const auto* invalid : {"1 + 2", "n =", "p = 'unclosed", "n = 1e", "n = 9223372036854775808",
                              "n = 0x_11", "p IN ()", "p IN (n)", "p LIKE n", "p LIKE '_' ESCAPE '!!'",
                              "p LIKE 'end!' ESCAPE '!'", "p = '2' -- comment", "n.thing = 1"}) {
    EXPECT_THROW(Selector::parse(invalid), InvalidSelectorException) << invalid;
  }
}

// REQ-70-7
TEST_F(MessageContractTest, GroupProperties) {
  std::cout << "REQ-70-7\n";
  Message source, parsed;
  parsed.fromBytes(source.toBytes());
  EXPECT_FALSE(parsed.hasProperty("JMSXGroupID"));
  EXPECT_FALSE(parsed.hasProperty("JMSXGroupSeq"));
  source.setStringProperty("JMSXGroupID", field("groupID", "string"));
  source.setIntProperty("JMSXGroupSeq", integer("groupSeq"));
  parsed.fromBytes(source.toBytes());
  EXPECT_EQ(parsed.property<property::String>("JMSXGroupID").value(), field("groupID", "string"));
  EXPECT_EQ(parsed.property<property::Int>("JMSXGroupSeq").value(), integer("groupSeq"));
}

// REQ-70-8
TEST_F(MessageContractTest, BinaryCorrelationId) {
  std::cout << "REQ-70-8\n";
  TextMessage source({}, "text"), target;
  for (const CorrelationId& variant : {CorrelationId{}, CorrelationId(std::string{}),
                                       CorrelationId(field("correlation", "string")), CorrelationId(BytesVector{}),
                                       CorrelationId(bytes("binaryCorrelation"))}) {
    source.jmsHeaders.setCorrelationId(variant);
    target.fromBytes(source.toBytes());
    EXPECT_EQ(target.jmsHeaders.getCorrelationId(), variant);
    EXPECT_EQ(Selector::parse("JMSCorrelationID IS NULL")->matches(target),
              !std::holds_alternative<std::string>(variant));
  }
  auto original = bytes("binaryCorrelation");
  source.jmsHeaders.setCorrelationId(original);
  original.front() = 1;
  auto returned = std::get<BytesVector>(source.jmsHeaders.getCorrelationId());
  returned.front() = 2;
  EXPECT_EQ(std::get<BytesVector>(source.jmsHeaders.getCorrelationId()), bytes("binaryCorrelation"));
}

// REQ-70-9
TEST_F(MessageContractTest, StorageV02Compatibility) {
  std::cout << "REQ-70-9\n";
  const auto old = bytes("storageV02");
  ASSERT_FALSE(old.empty());
  ASSERT_EQ(old.front(), 0x02);
  TextMessage original;
  original.fromBytes(old);
  EXPECT_EQ(original.uuid.toString(), "00112233-4455-6677-8899-aabbccddeeff");
  EXPECT_EQ(original.text(), field("text", "string"));
  EXPECT_EQ(original.reliability, Message::PERSISTENT);
  EXPECT_EQ(original.jmsHeaders.messageId, "ID:custom-42");
  EXPECT_EQ(original.jmsHeaders.timestamp, 1700000000123LL);
  EXPECT_EQ(original.jmsHeaders.expiration, 1700000999999LL);
  EXPECT_EQ(original.jmsHeaders.deliveryTime, integer64("deliveryTime"));
  EXPECT_EQ(original.jmsHeaders.priority, integer("priority"));
  EXPECT_TRUE(original.jmsHeaders.redelivered);
  EXPECT_EQ(original.jmsHeaders.deliveryCount, 3);
  EXPECT_EQ(original.jmsHeaders.replyTo, field("replyTo", "uri"));
  EXPECT_EQ(original.jmsHeaders.correlationId, field("correlation", "string"));
  EXPECT_EQ(original.jmsHeaders.type, field("messageType", "string"));
  EXPECT_EQ(original.property<property::Byte>("b").value(), -12);
  EXPECT_EQ(original.property<property::Long>("big").value(), 9007199254740993LL);
  EXPECT_TRUE(original.jmsHeaders.destination.empty());

  original.jmsHeaders.setCorrelationId(bytes("binaryCorrelation"));
  original.jmsHeaders.setType(std::string{});
  original.clearData();
  original.setNullProperty(field("nullProperty", "name"));
  const auto current = original.toBytes();
  ASSERT_EQ(current.front(), 0x03);
  TextMessage updated;
  updated.fromBytes(current);
  EXPECT_EQ(updated.jmsHeaders.getCorrelationId(), CorrelationId(bytes("binaryCorrelation")));
  EXPECT_TRUE(updated.jmsHeaders.explicitEmptyType);
  EXPECT_TRUE(updated.isTextNull());
  EXPECT_TRUE(updated.property<property::String>(field("nullProperty", "name")).isNull());

  if (const auto* path = std::getenv("TINY_MQ_V03_PATH")) {
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(current.data()), current.size());
    ASSERT_TRUE(output.good());
  }
}
}  // namespace
