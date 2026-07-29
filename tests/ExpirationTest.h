//
// JMSExpiration tests for tiny-mq (JMS spec 44 — message expiration sweep).
//

#ifndef TINY_MQ_TESTS_EXPIRATION_TEST_H_
#define TINY_MQ_TESTS_EXPIRATION_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class ExpirationTest : public ::testing::Test {
 public:
  ExpirationTest() = default;
  ~ExpirationTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_EXPIRATION_TEST_H_
