//
// Connection lifecycle / clientID / ExceptionListener / ConnectionMetaData
// tests for tiny-mq (JMS spec 03).
//

#include "ConnectionLifecycleTest.h"
#include "Connection.h"
#include "Exceptions.h"
#include "Exchange.h"
#include "Session.h"
#include "TestHelper.h"
#include <stdexcept>

using tiny_mq::Connection;
using tiny_mq::IllegalStateException;
using tiny_mq::Session;

void LifecycleTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void LifecycleTest::TearDown() { _exchange.reset(); }

void ExceptionListenerTest::SetUp() { _exchange = std::make_unique<tiny_mq::Exchange>("./tiny-mq"); }
void ExceptionListenerTest::TearDown() { _exchange.reset(); }

// clientID may be set exactly once; a second call throws IllegalStateException.
TEST_F(LifecycleTest, testDoubleSetClientIDThrows) {
  Connection connection(*_exchange);
  connection.setClientID("client-A");
  EXPECT_EQ("client-A", connection.clientID());
  EXPECT_THROW(connection.setClientID("client-B"), IllegalStateException);
  EXPECT_EQ("client-A", connection.clientID());  // unchanged
}

// clientID must be set before the connection is used (creating a session counts).
TEST_F(LifecycleTest, testSetClientIDAfterUseThrows) {
  Connection connection(*_exchange);
  connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE);
  EXPECT_THROW(connection.setClientID("late"), IllegalStateException);
}

// close() is idempotent — repeated calls are safe no-ops.
TEST_F(LifecycleTest, testCloseIsIdempotent) {
  Connection connection(*_exchange);
  EXPECT_FALSE(connection.isClosed());
  connection.close();
  EXPECT_TRUE(connection.isClosed());
  EXPECT_NO_THROW(connection.close());
  EXPECT_NO_THROW(connection.close());
  EXPECT_TRUE(connection.isClosed());
}

// Any mutating operation after close() throws IllegalStateException.
TEST_F(LifecycleTest, testOperationsAfterCloseThrow) {
  Connection connection(*_exchange);
  connection.close();

  EXPECT_THROW(connection.createSession(Session::AcknowledgeMode::AUTO_ACKNOWLEDGE),
               IllegalStateException);
  EXPECT_THROW(connection.start(), IllegalStateException);
  EXPECT_THROW(connection.stop(), IllegalStateException);
  EXPECT_THROW(connection.setClientID("x"), IllegalStateException);
  EXPECT_THROW(connection.setExceptionListener([](const std::exception &) {}),
               IllegalStateException);
}

// metadata() is informational and remains available after close().
TEST_F(LifecycleTest, testMetadataAvailableAndStableAfterClose) {
  Connection connection(*_exchange);
  auto md = connection.metadata();
  EXPECT_EQ("tiny-mq", md.providerName);
  EXPECT_EQ("2.0", md.jmsVersion);
  EXPECT_EQ(2, md.jmsMajorVersion);
  EXPECT_EQ(0, md.jmsMinorVersion);

  connection.close();
  EXPECT_NO_THROW(connection.metadata());
  EXPECT_EQ("tiny-mq", connection.metadata().providerName);
}

// A registered ExceptionListener fires on a simulated connection-level failure.
TEST_F(ExceptionListenerTest, testListenerFiresOnFailure) {
  Connection connection(*_exchange);
  std::string captured;
  connection.setExceptionListener([&captured](const std::exception &ex) { captured = ex.what(); });

  connection.onException(std::runtime_error("simulated network drop"));
  EXPECT_EQ("simulated network drop", captured);
}

// onException with no listener registered is a safe no-op.
TEST_F(ExceptionListenerTest, testNoListenerIsNoop) {
  Connection connection(*_exchange);
  EXPECT_NO_THROW(connection.onException(std::runtime_error("ignored")));
}
