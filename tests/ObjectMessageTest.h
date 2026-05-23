//
// ObjectMessage tests for tiny-mq (JMS spec 11)
//

#ifndef TINY_MQ_TESTS_OBJECT_MESSAGE_TEST_H_
#define TINY_MQ_TESTS_OBJECT_MESSAGE_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class ObjectMessageTest : public ::testing::Test {
 public:
  ObjectMessageTest() = default;
  ~ObjectMessageTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_OBJECT_MESSAGE_TEST_H_
