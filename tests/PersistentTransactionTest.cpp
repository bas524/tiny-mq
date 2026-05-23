//
// Created for persistent transaction tests in tiny-mq
//

#include "PersistentTransactionTest.h"
#include "Exchange.h"
#include "Session.h"
#include "Connection.h"
#include "TestHelper.h"
#include <Poco/File.h>

using tiny_mq::Consumer;
using tiny_mq::Destination;
using tiny_mq::Message;
using tiny_mq::Producer;
using tiny_mq::Session;
using tiny_mq::Connection;
using tiny_mq::TextMessage;

void PersistentTransactionTest::SetUp() { 
    _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq-persistent-test");
}

void PersistentTransactionTest::TearDown() { 
    _exchange.reset();
    // Clean up test directory
    Poco::File f("./tiny-mq-persistent-test");
    if (f.exists()) {
        f.remove(true);
    }
}

TEST_F(PersistentTransactionTest, testPersistentSendReceiveTransactedBatches) {
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
    std::generate(msg.begin(), msg.end(), [&ii]() { return "Persistent Batch Message " + std::to_string(ii++); });

    for (size_t j = 0; j < batchCount; j++) {
        for (size_t i = 0; i < batchSize; i++) {
            TextMessage message = session.createTextMessage(msg[i], Message::PERSISTENT);
            EXPECT_NO_THROW(producer->send(message)) << "Send should not throw an exception here.";
        }

        EXPECT_NO_THROW(session.commit()) << "Session Commit should not throw an exception here:";

        for (size_t i = 0; i < batchSize; i++) {
            TextMessage::Ptr pmessage;
            EXPECT_NO_THROW(pmessage = Message::As<TextMessage>(consumer->recv())) << "Receive Shouldn't throw a Message here:";

            EXPECT_NE(pmessage, nullptr) << "Failed to receive all messages in batch";
            EXPECT_TRUE(msg[i] == pmessage->text());
            EXPECT_EQ(pmessage->reliability, Message::PERSISTENT) << "Message should be persistent";
        }

        EXPECT_NO_THROW(session.commit()) << "Session Commit should not throw an exception here:";
    }
}

TEST_F(PersistentTransactionTest, testPersistentSendRollback) {
    // Create CMS Object for Comms
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    EXPECT_NE(destination, nullptr);

    Producer::Ptr producer = session.createProducer(destination);
    EXPECT_NE(producer, nullptr);

    Consumer::Ptr consumer = session.createConsumer(destination);
    EXPECT_NE(consumer, nullptr);

    TextMessage outbound1 = session.createTextMessage("First Persistent Message", Message::PERSISTENT);
    TextMessage outbound2 = session.createTextMessage("Second Persistent Message", Message::PERSISTENT);
    
    // sends a message
    EXPECT_NO_THROW(producer->send(outbound1));
    EXPECT_NO_THROW(session.commit());

    // sends a message that gets rollbacked
    TextMessage rollback = session.createTextMessage("I'm going to get rolled back.", Message::PERSISTENT);
    EXPECT_NO_THROW(producer->send(rollback));

    EXPECT_NO_THROW(session.rollback());

    // sends a message
    EXPECT_NO_THROW(producer->send(outbound2));
    EXPECT_NO_THROW(session.commit());

    // receives the first message
    TextMessage::Ptr inbound1;
    EXPECT_NO_THROW(inbound1 = Message::As<TextMessage>(consumer->recv()));

    // receives the second message
    TextMessage::Ptr inbound2;
    EXPECT_NO_THROW(inbound2 = Message::As<TextMessage>(consumer->recv()));

    // validates that the rollbacked was not consumed
    EXPECT_NO_THROW(session.commit());

    EXPECT_NE(inbound1, nullptr);
    EXPECT_EQ(outbound1.text(), inbound1->text());
    EXPECT_EQ(inbound1->reliability, Message::PERSISTENT);

    EXPECT_NE(inbound2, nullptr);
    EXPECT_EQ(outbound2.text(), inbound2->text());
    EXPECT_EQ(inbound2->reliability, Message::PERSISTENT);
    
    // Should not receive the rolled back message
    TextMessage::Ptr inbound3;
    EXPECT_NO_THROW(inbound3 = Message::As<TextMessage>(consumer->recv(100))); // Short timeout
    EXPECT_EQ(nullptr, inbound3) << "Rolled back message should not be received";
}

TEST_F(PersistentTransactionTest, testPersistentSendAndAcknowledgeInSameTransaction) {
    // Test where message is sent and acknowledged in same transaction
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    EXPECT_NE(destination, nullptr);

    Producer::Ptr producer = session.createProducer(destination);
    EXPECT_NE(producer, nullptr);

    Consumer::Ptr consumer = session.createConsumer(destination);
    EXPECT_NE(consumer, nullptr);

    TextMessage message = session.createTextMessage("Persistent message to be acknowledged", Message::PERSISTENT);
    
    // Send message in transaction and commit to make it visible to consumer
    EXPECT_NO_THROW(producer->send(message));
    EXPECT_NO_THROW(session.commit());

    // Receive and acknowledge in a new transaction
    TextMessage::Ptr received;
    EXPECT_NO_THROW(received = Message::As<TextMessage>(consumer->recv()));
    ASSERT_NE(received, nullptr);
    EXPECT_EQ(message.text(), received->text());

    EXPECT_NO_THROW(consumer->acknowledgeOn(*received));

    // Commit the receive transaction - message should be removed from storage
    EXPECT_NO_THROW(session.commit());
    
    // Should not receive message again
    TextMessage::Ptr shouldBeNull;
    EXPECT_NO_THROW(shouldBeNull = Message::As<TextMessage>(consumer->recv(100)));
    EXPECT_EQ(nullptr, shouldBeNull) << "Message should not be received after acknowledgement";
}

TEST_F(PersistentTransactionTest, testMixedPersistentNonPersistentInTransaction) {
    // Test mixing persistent and non-persistent messages in same transaction
    Connection session_conn(*_exchange);
    Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
    Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, CurrentTestName);
    EXPECT_NE(destination, nullptr);

    Producer::Ptr producer = session.createProducer(destination);
    EXPECT_NE(producer, nullptr);

    Consumer::Ptr consumer = session.createConsumer(destination);
    EXPECT_NE(consumer, nullptr);

    TextMessage persistentMsg = session.createTextMessage("Persistent Message", Message::PERSISTENT);
    TextMessage nonPersistentMsg = session.createTextMessage("Non-Persistent Message", Message::NOT_PERSISTENT);
    
    // Send both messages in transaction
    EXPECT_NO_THROW(producer->send(persistentMsg));
    EXPECT_NO_THROW(producer->send(nonPersistentMsg));
    
    EXPECT_NO_THROW(session.commit());
    
    // Receive both messages
    TextMessage::Ptr received1;
    TextMessage::Ptr received2;
    
    EXPECT_NO_THROW(received1 = Message::As<TextMessage>(consumer->recv()));
    EXPECT_NO_THROW(received2 = Message::As<TextMessage>(consumer->recv()));
    
    EXPECT_NE(received1, nullptr);
    EXPECT_NE(received2, nullptr);
    
    // Check that one is persistent and one is not
    // Note: order might not be guaranteed, so check both
    bool foundPersistent = false;
    bool foundNonPersistent = false;
    
    if (received1->reliability == Message::PERSISTENT) {
        foundPersistent = true;
        EXPECT_EQ(persistentMsg.text(), received1->text());
    } else {
        foundNonPersistent = true;
        EXPECT_EQ(nonPersistentMsg.text(), received1->text());
    }
    
    if (received2->reliability == Message::PERSISTENT) {
        foundPersistent = true;
        EXPECT_EQ(persistentMsg.text(), received2->text());
    } else {
        foundNonPersistent = true;
        EXPECT_EQ(nonPersistentMsg.text(), received2->text());
    }
    
    EXPECT_TRUE(foundPersistent) << "Should have received persistent message";
    EXPECT_TRUE(foundNonPersistent) << "Should have received non-persistent message";
    
    EXPECT_NO_THROW(session.commit());
}

TEST_F(PersistentTransactionTest, testTransactionRecoveryAfterRestart) {
    // Test that transactions survive restart (persistent messages)
    std::string testName = CurrentTestName;
    
    // First session - send message but don't commit
    {
        Connection session_conn(*_exchange);
        Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
        Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, testName);
        EXPECT_NE(destination, nullptr);

        Producer::Ptr producer = session.createProducer(destination);
        EXPECT_NE(producer, nullptr);

        TextMessage message = session.createTextMessage("Persistent message before restart", Message::PERSISTENT);
        EXPECT_NO_THROW(producer->send(message));
        
        // Don't commit - session destructor will rollback
    }
    
    // Recreate exchange (simulating restart)
    _exchange.reset();
    _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq-persistent-test");
    
    // New session - should not see the uncommitted message
    {
        Connection session_conn(*_exchange);
        Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
        Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, testName);
        EXPECT_NE(destination, nullptr);

        Consumer::Ptr consumer = session.createConsumer(destination);
        EXPECT_NE(consumer, nullptr);

        // Should not receive the uncommitted message
        TextMessage::Ptr received;
        EXPECT_NO_THROW(received = Message::As<TextMessage>(consumer->recv(100)));
        EXPECT_EQ(nullptr, received) << "Uncommitted message should not be visible after restart";
    }
}

TEST_F(PersistentTransactionTest, testCommittedTransactionSurvivesRestart) {
    // Test that committed transactions survive restart
    std::string testName = CurrentTestName;
    
    // First session - send message and commit
    {
        Connection session_conn(*_exchange);
        Session &session = session_conn.createSession(Session::AcknowledgeMode::SESSION_TRANSACTED);
        Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, testName);
        EXPECT_NE(destination, nullptr);

        Producer::Ptr producer = session.createProducer(destination);
        EXPECT_NE(producer, nullptr);

        TextMessage message = session.createTextMessage("Persistent committed message", Message::PERSISTENT);
        EXPECT_NO_THROW(producer->send(message));
        
        EXPECT_NO_THROW(session.commit());
    }
    
    // Recreate exchange (simulating restart)
    _exchange.reset();
    _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq-persistent-test");
    
    // New session - should see the committed message
    {
        Connection session_conn(*_exchange);
        Session &session = session_conn.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
        Destination::Ptr destination = session.createDestination(tiny_mq::destination::Queue, testName);
        EXPECT_NE(destination, nullptr);

        Consumer::Ptr consumer = session.createConsumer(destination);
        EXPECT_NE(consumer, nullptr);

        // Should receive the committed message
        TextMessage::Ptr received;
        EXPECT_NO_THROW(received = Message::As<TextMessage>(consumer->recv()));
        EXPECT_NE(received, nullptr);
        EXPECT_EQ("Persistent committed message", received->text());
        EXPECT_EQ(Message::PERSISTENT, received->reliability);
    }
}