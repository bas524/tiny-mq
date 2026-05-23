//
// Created by Alexander Bychuk on 16.08.2023.
//

#ifndef TINY_MQ_TESTS_TRANSACTIONTEST_H_
#define TINY_MQ_TESTS_TRANSACTIONTEST_H_

#include <gtest/gtest.h>
namespace tiny_mq {
class Exchange;
}

class TransactionTest : public ::testing::Test {
  public:
  TransactionTest() = default;
  ~TransactionTest() override = default;

  protected:
  std::unique_ptr<tiny_mq::Exchange> _exchange;
  void SetUp() override;
  void TearDown() override;
  
  static const size_t batchCount = 10;
  static const size_t batchSize = 20;
};

#endif  // TINY_MQ_TESTS_TRANSACTIONTEST_H_
