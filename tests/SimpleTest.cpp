//
// Created by Alexander Bychuk on 12.02.2022.
//

#include "SimpleTest.h"
#include "Exchange.h"
#include "TextMessage.h"
#include <Poco/UUIDGenerator.h>

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::TextMessage;

void SimpleTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void SimpleTest::TearDown() { _exchange.reset(); }

////////////////////////////////////////////////////////////////////////////////
TEST_F(SimpleTest, testSendRecv) {
  Destination::Ptr destination = _exchange->create(tiny_mq::destination::Queue, "test");
  EXPECT_NE(destination, nullptr);

  Consumer::Ptr consumer = destination->createConsumer();
  EXPECT_NE(consumer, nullptr);
  Producer::Ptr producer = destination->createProducer();
  EXPECT_NE(producer, nullptr);

  TextMessage message1("TEST MESSAGE 1");
  message1.uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  message1.reliability = Message::NOT_PERSISTENT;

  TextMessage message2("TEST MESSAGE 2");
  message2.uuid = Poco::UUIDGenerator::defaultGenerator().createRandom();
  message2.reliability = Message::NOT_PERSISTENT;

  producer->send(message1);
  producer->send(message2);

  TextMessage::Ptr pmessage1 = Message::As<TextMessage>(consumer->recv());
  EXPECT_NE(pmessage1, nullptr);
  consumer->acknowledgeOn(*pmessage1);
  TextMessage::Ptr pmessage2 = Message::As<TextMessage>(consumer->recv());
  EXPECT_NE(pmessage2, nullptr);
  consumer->acknowledgeOn(*pmessage2);

  EXPECT_EQ(message1.text(), pmessage1->text());
  EXPECT_EQ(message2.text(), pmessage2->text());
}
