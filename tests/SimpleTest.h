//
// Created by Alexander Bychuk on 12.02.2022.
//

#ifndef TINY_MQ_TESTS_SIMPLETEST_H_
#define TINY_MQ_TESTS_SIMPLETEST_H_
#include <gtest/gtest.h>
namespace tiny_mq {
class Exchange;
}

class SimpleTest : public ::testing::Test {
 public:
  SimpleTest() = default;
  ~SimpleTest() override = default;

 protected:
  std::unique_ptr<tiny_mq::Exchange> _exchange;
  void SetUp() override;
  void TearDown() override;
};

#endif  // TINY_MQ_TESTS_SIMPLETEST_H_
