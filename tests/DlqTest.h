//
// Redelivery counter + dead-letter queue tests for tiny-mq (spec 24).
//

#ifndef TINY_MQ_TESTS_DLQ_TEST_H_
#define TINY_MQ_TESTS_DLQ_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class DlqTest : public ::testing::Test {
 public:
  DlqTest() = default;
  ~DlqTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_DLQ_TEST_H_
