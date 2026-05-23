//
// DUPS_OK_ACKNOWLEDGE tests for tiny-mq (JMS spec 22)
//

#ifndef TINY_MQ_TESTS_DUPS_OK_ACK_TEST_H_
#define TINY_MQ_TESTS_DUPS_OK_ACK_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class DupsOkAckTest : public ::testing::Test {
 public:
  DupsOkAckTest() = default;
  ~DupsOkAckTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_DUPS_OK_ACK_TEST_H_
