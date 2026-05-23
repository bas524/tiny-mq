//
// Created by Alexander Bychuk on 26.03.2022.
//

#include "MapMessageTest.h"
#include "MapMessage.h"
#include "Session.h"
#include "Connection.h"

using tiny_mq::BytesVector;
using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::MapMessage;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::property::Bool;
using tiny_mq::property::Byte;
using tiny_mq::property::Bytes;
using tiny_mq::property::Char;
using tiny_mq::property::Double;
using tiny_mq::property::Float;
using tiny_mq::property::Int;
using tiny_mq::property::Long;
using tiny_mq::property::Short;
using tiny_mq::property::String;

void MapMessageTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void MapMessageTest::TearDown() { _exchange.reset(); }

TEST_F(MapMessageTest, testClearBody) {
  String testText = std::string("This is some test Text");

  MapMessage msg;

  EXPECT_NO_THROW(msg.set("strProp1", testText));

  String result = msg.get<String>("strProp1");

  EXPECT_EQ(result, testText);

  EXPECT_NO_THROW(msg.clearData());

  EXPECT_TRUE(msg.empty());
}

TEST_F(MapMessageTest, testToJson) {
  MapMessage msg;

  EXPECT_NO_THROW(msg.setProperty("prop-int", tiny_mq::property::Int(22)));
  EXPECT_NO_THROW(msg.setProperty("prop-null-bytes", tiny_mq::property::Bytes()));
  EXPECT_NO_THROW(msg.set("val-int", tiny_mq::property::Int(33)));
  EXPECT_NO_THROW(msg.set("val-null-bytes", tiny_mq::property::Bytes()));

  std::string json;
  EXPECT_NO_THROW(json = tiny_mq::message::dump(msg));
  const std::string s = R"({
 "data": {
  "val-int": {
   "type": "INTEGER",
   "value": 33
  },
  "val-null-bytes": {
   "type": "BYTE_ARRAY",
   "value": null
  }
 },
 "number": 0,
 "persistentInfo": {
  "payload": {
   "dataPath": "",
   "propertiesPath": ""
  },
  "sent": {
   "dataPath": "",
   "propertiesPath": ""
  },
  "transaction": {
   "dataPath": "",
   "propertiesPath": ""
  }
 },
 "properties": {
  "prop-int": {
   "type": "INTEGER",
   "value": 22
  },
  "prop-null-bytes": {
   "type": "BYTE_ARRAY",
   "value": null
  }
 },
 "reliability": "NOT_PERSISTENT",
 "uuid": "00000000-0000-0000-0000-000000000000"
})";
  EXPECT_EQ(json, s);
}
TEST_F(MapMessageTest, testSendRecvCloneMapMessage) {
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession();
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, "testSendRecvCloneMapMessage");
  EXPECT_NE(destination, nullptr);

  Consumer::Ptr consumer = session.createConsumer(destination);
  EXPECT_NE(consumer, nullptr);
  Producer::Ptr producer = session.createProducer(destination);
  EXPECT_NE(producer, nullptr);

  MapMessage message = session.createMapMessage();

  EXPECT_TRUE(message.names().empty());
  EXPECT_FALSE(message.has("Something"));

  BytesVector data;

  data.push_back(2);
  data.push_back(4);
  data.push_back(8);
  data.push_back(16);
  data.push_back(32);

  message.set("boolean", Bool(false));
  message.set("byte", Byte(127));
  message.set("char", Char('a'));
  message.set("short", Short(32000));
  message.set("int", Int(6789999));
  message.set("long", Long(0xFFFAAA33345LL));
  message.set("float", Float(0.000012F));
  message.set("double", Double(64.54654));
  message.set("bytes", Bytes(data));

  EXPECT_NO_THROW(producer->send(message));

  MapMessage::Ptr pmessage1 = Message::As<MapMessage>(consumer->recv());
  EXPECT_NE(pmessage1, nullptr);

  EXPECT_EQ(pmessage1->valueType("boolean"), tiny_mq::property::BOOLEAN_TYPE);
  EXPECT_FALSE(pmessage1->get<Bool>("boolean").value());
  EXPECT_EQ(pmessage1->valueType("byte"), tiny_mq::property::BYTE_TYPE);
  EXPECT_EQ(pmessage1->get<Byte>("byte").value(), 127);
  EXPECT_EQ(pmessage1->valueType("char"), tiny_mq::property::CHAR_TYPE);
  EXPECT_EQ(pmessage1->get<Char>("char").value(), 'a');
  EXPECT_EQ(pmessage1->valueType("short"), tiny_mq::property::SHORT_TYPE);
  EXPECT_EQ(pmessage1->get<Short>("short").value(), 32000);
  EXPECT_EQ(pmessage1->valueType("int"), tiny_mq::property::INTEGER_TYPE);
  EXPECT_EQ(pmessage1->get<Int>("int").value(), 6789999);
  EXPECT_EQ(pmessage1->valueType("long"), tiny_mq::property::LONG_TYPE);
  EXPECT_EQ(pmessage1->get<Long>("long").value(), 0xFFFAAA33345LL);
  EXPECT_EQ(pmessage1->valueType("float"), tiny_mq::property::FLOAT_TYPE);
  EXPECT_EQ(pmessage1->get<Float>("float").value(), 0.000012F);
  EXPECT_EQ(pmessage1->valueType("double"), tiny_mq::property::DOUBLE_TYPE);
  EXPECT_EQ(pmessage1->get<Double>("double").value(), 64.54654);
  EXPECT_EQ(pmessage1->valueType("bytes"), tiny_mq::property::BYTE_ARRAY_TYPE);
  EXPECT_EQ(pmessage1->get<Bytes>("bytes").value(), data);

  MapMessage::Ptr clonedMessage = Message::As<MapMessage>(pmessage1->copy());
  EXPECT_TRUE(clonedMessage != nullptr);

  EXPECT_FALSE(clonedMessage->get<Bool>("boolean").value());
  EXPECT_EQ(clonedMessage->get<Byte>("byte").value(), 127);
  EXPECT_EQ(clonedMessage->get<Char>("char").value(), 'a');
  EXPECT_EQ(clonedMessage->get<Short>("short").value(), 32000);
  EXPECT_EQ(clonedMessage->get<Int>("int").value(), 6789999);
  EXPECT_EQ(clonedMessage->get<Long>("long").value(), 0xFFFAAA33345LL);
  EXPECT_EQ(clonedMessage->get<Float>("float").value(), 0.000012F);
  EXPECT_EQ(clonedMessage->get<Double>("double").value(), 64.54654);
  EXPECT_EQ(clonedMessage->get<Bytes>("bytes").value(), data);
}
