//
// Created by Alexander Bychuk on 16.08.2023.
//

#include "TransactionTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TestHelper.h"

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::TextMessage;

void TransactionTest::SetUp() {
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
  _exchange = std::make_unique<tiny_mq::Exchange>(CurrentTestSuiteStorageDir());
}
void TransactionTest::TearDown() {
  _exchange.reset();
  RemoveTestStorageDir(CurrentTestSuiteStorageDir());
}

TEST_F(TransactionTest, testSendReceiveTransactedBatches) {
  // Create CMS Object for Comms
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  EXPECT_NE(destination, nullptr);

  Producer::Ptr producer = session.createProducer(destination);
  EXPECT_NE(producer, nullptr);

  Consumer::Ptr consumer = session.createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  std::vector<std::string> msg(batchSize);
  size_t ii = 0;
  std::generate(msg.begin(), msg.end(), [&ii]() { return "Batch Message " + std::to_string(ii++); });

  for (size_t j = 0; j < batchCount - 8; j++) {
    for (size_t i = 0; i < batchSize; i++) {
      TextMessage message = session.createTextMessage(msg[i], Message::NOT_PERSISTENT);
      EXPECT_NO_THROW(producer->send(message)) << "Send should not throw an exception here.";
    }

    EXPECT_NO_THROW(session.commit()) << "Session Commit should not throw an exception here:";

    for (size_t i = 0; i < batchSize; i++) {
      TextMessage::Ptr pmessage;
      EXPECT_NO_THROW(pmessage = Message::As<TextMessage>(consumer->recv())) << "Receive Shouldn't throw a Message here:";

      EXPECT_NE(pmessage, nullptr) << "Failed to receive all messages in batch";
      EXPECT_TRUE(msg[i] == pmessage->text());
    }

    EXPECT_NO_THROW(session.commit()) << "Session Commit should not throw an exception here:";
  }
}
TEST_F(TransactionTest, testSendRollback) {
  // Create CMS Object for Comms
  Connection session_conn(*_exchange);
  Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
  EXPECT_NE(destination, nullptr);

  Producer::Ptr producer = session.createProducer(destination);
  EXPECT_NE(producer, nullptr);

  Consumer::Ptr consumer = session.createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  TextMessage outbound1 = session.createTextMessage("First Message", Message::NOT_PERSISTENT);
  TextMessage outbound2 = session.createTextMessage("Second Message", Message::NOT_PERSISTENT);
  // sends a message
  EXPECT_NO_THROW(producer->send(outbound1));
  EXPECT_NO_THROW(session.commit());

  // sends a message that gets rollbacked
  TextMessage rollback = session.createTextMessage("I'm going to get rolled back.");
  EXPECT_NO_THROW(producer->send(rollback));

  EXPECT_NO_THROW(session.rollback());

  // sends a message
  EXPECT_NO_THROW(producer->send(outbound2));
  EXPECT_NO_THROW(session.commit());

  // receives the first messag

  TextMessage::Ptr inbound1;
  EXPECT_NO_THROW(inbound1 = Message::As<TextMessage>(consumer->recv()));

  // receives the second message
  TextMessage::Ptr inbound2;
  EXPECT_NO_THROW(inbound2 = Message::As<TextMessage>(consumer->recv()));

  // validates that the rollbacked was not consumed
  EXPECT_NO_THROW(session.commit());

  EXPECT_NE(inbound1, nullptr);

  EXPECT_EQ(outbound1.text(), inbound1->text());

  EXPECT_NE(inbound2, nullptr);

  EXPECT_EQ(outbound2.text(), inbound2->text()) << "invalid order : ou1-id[" << outbound1.uuid.toString() << "] : in1-id["
                                                << inbound1->uuid.toString() << "]\n"
                                                << "invalid order : ou2-id[" << outbound2.uuid.toString() << "] : in2-id["
                                                << inbound2->uuid.toString() << "]";
}
TEST_F(TransactionTest, testSendRollbackCommitRollback) {
  // Create CMS Object for Comms
  Connection connection(*_exchange);
  Session *session = &connection.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr destination = session->createDestination(tiny_mq::destination::Queue, CurrentTestName);
  EXPECT_NE(destination, nullptr);

  Producer::Ptr producer = session->createProducer(destination);
  EXPECT_NE(producer, nullptr);

  Consumer::Ptr consumer = session->createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  TextMessage outbound1(session->createTextMessage("First Message", Message::NOT_PERSISTENT));
  TextMessage outbound2(session->createTextMessage("Second Message", Message::NOT_PERSISTENT));

  // sends them and then rolls back.
  EXPECT_NO_THROW(producer->send(outbound1));
  EXPECT_NO_THROW(producer->send(outbound2));
  EXPECT_NO_THROW(session->rollback());

  // Send one and commit.
  EXPECT_NO_THROW(producer->send(outbound1));
  EXPECT_NO_THROW(session->commit());

  // receives the first message
  TextMessage::Ptr inbound1;
  EXPECT_NO_THROW(inbound1 = Message::As<TextMessage>(consumer->recv()));

  TextMessage::Ptr inboundEmpty;
  EXPECT_NO_THROW(inboundEmpty = Message::As<TextMessage>(consumer->recv(100)));
  EXPECT_EQ(nullptr, inboundEmpty) << "must be empty, but : " << inboundEmpty->text();

  ASSERT_NE(inbound1, nullptr);

  EXPECT_EQ(outbound1.text(), inbound1->text());

  session->rollback();

  EXPECT_NO_THROW(inbound1 = Message::As<TextMessage>(consumer->recv()));

  EXPECT_NO_THROW(inboundEmpty = Message::As<TextMessage>(consumer->recv(100)));
  EXPECT_EQ(nullptr, inboundEmpty) << "must be empty, but : " << inboundEmpty->text();

  EXPECT_EQ(outbound1.text(), inbound1->text());

  // validates that the rollbacked was not consumed
  session->commit();
}
TEST_F(TransactionTest, testSendSessionClose) {
  auto connection = std::make_unique<Connection>(*_exchange);
  Session *session = &connection->createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  Destination::Ptr destination = session->createDestination(tiny_mq::destination::Queue, CurrentTestName);
  EXPECT_NE(destination, nullptr);

  Producer::Ptr producer = session->createProducer(destination);
  EXPECT_NE(producer, nullptr);

  Consumer::Ptr consumer = session->createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  TextMessage outbound1(session->createTextMessage("First Message", Message::NOT_PERSISTENT));
  TextMessage outbound2(session->createTextMessage("Second Message", Message::NOT_PERSISTENT));

  // sends a message
  EXPECT_NO_THROW(producer->send(outbound1));
  EXPECT_NO_THROW(session->commit());

  // sends a message that gets rolled back
  TextMessage rollback(session->createTextMessage("I'm going to get rolled back.", Message::NOT_PERSISTENT));
  EXPECT_NO_THROW(producer->send(rollback));

  // close connection — destructor unregisters all consumers/producers and sessions automatically
  EXPECT_NO_THROW(consumer.reset());
  EXPECT_NO_THROW(producer.reset());
  EXPECT_NO_THROW(connection.reset());

  connection = std::make_unique<Connection>(*_exchange);
  session = &connection->createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
  destination = session->createDestination(tiny_mq::destination::Queue, CurrentTestName);
  EXPECT_NE(destination, nullptr);

  producer = session->createProducer(destination);
  EXPECT_NE(producer, nullptr);

  consumer = session->createConsumer(destination);
  EXPECT_NE(consumer, nullptr);

  // sends a message
  EXPECT_NO_THROW(producer->send(outbound2));
  EXPECT_NO_THROW(session->commit());

  // receives the first message
  TextMessage::Ptr inbound1;
  EXPECT_NO_THROW(inbound1 = Message::As<TextMessage>(consumer->recv()));

  // receives the second message
  TextMessage::Ptr inbound2;
  EXPECT_NO_THROW(inbound2 = Message::As<TextMessage>(consumer->recv()));

  // validates that the rolled back was not consumed
  EXPECT_NO_THROW(session->commit());

  EXPECT_NE(inbound1, nullptr);

  EXPECT_EQ(outbound1.text(), inbound1->text());

  EXPECT_NE(inbound2, nullptr);

  EXPECT_EQ(outbound2.text(), inbound2->text());
}
