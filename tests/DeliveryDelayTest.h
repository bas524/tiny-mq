//
// DeliveryDelay tests for tiny-mq (JMS 2.0 § 7.8 — spec 13).
//

#ifndef TINY_MQ_TESTS_DELIVERY_DELAY_TEST_H_
#define TINY_MQ_TESTS_DELIVERY_DELAY_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class DeliveryDelayTest : public ::testing::Test {
 public:
  DeliveryDelayTest() = default;
  ~DeliveryDelayTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_DELIVERY_DELAY_TEST_H_
