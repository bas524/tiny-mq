//
// Per-send DeliveryMode / Priority / TTL tests for tiny-mq (JMS spec 12).
//

#ifndef TINY_MQ_TESTS_SEND_OPTIONS_TEST_H_
#define TINY_MQ_TESTS_SEND_OPTIONS_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class SendOptionsTest : public ::testing::Test {
 public:
  SendOptionsTest() = default;
  ~SendOptionsTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_SEND_OPTIONS_TEST_H_
