//
// Disable MessageID / Timestamp tests for tiny-mq (JMS spec 14)
//

#ifndef TINY_MQ_TESTS_DISABLE_HEADERS_TEST_H_
#define TINY_MQ_TESTS_DISABLE_HEADERS_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class DisableHeadersTest : public ::testing::Test {
 public:
  DisableHeadersTest() = default;
  ~DisableHeadersTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_DISABLE_HEADERS_TEST_H_
