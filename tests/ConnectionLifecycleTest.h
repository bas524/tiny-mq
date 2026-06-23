//
// Connection lifecycle / clientID / ExceptionListener / ConnectionMetaData
// tests for tiny-mq (JMS spec 03).
//

#ifndef TINY_MQ_TESTS_CONNECTION_LIFECYCLE_TEST_H_
#define TINY_MQ_TESTS_CONNECTION_LIFECYCLE_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class LifecycleTest : public ::testing::Test {
 public:
  LifecycleTest() = default;
  ~LifecycleTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

class ExceptionListenerTest : public ::testing::Test {
 public:
  ExceptionListenerTest() = default;
  ~ExceptionListenerTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_CONNECTION_LIFECYCLE_TEST_H_
