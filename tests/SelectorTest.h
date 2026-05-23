//
// Created by Alexander Bychuk on 12.04.2026.
//

#ifndef TINY_MQ_TESTS_SELECTORTEST_H_
#define TINY_MQ_TESTS_SELECTORTEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class SelectorTest : public ::testing::Test {
 public:
  SelectorTest() = default;
  ~SelectorTest() override = default;

 protected:
  std::unique_ptr<tiny_mq::Exchange> _exchange;
  void SetUp() override;
  void TearDown() override;
};

#endif  // TINY_MQ_TESTS_SELECTORTEST_H_
