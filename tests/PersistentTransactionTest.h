//
// Created for persistent transaction tests in tiny-mq
//

#ifndef TINY_MQ_TESTS_PERSISTENT_TRANSACTION_TEST_H_
#define TINY_MQ_TESTS_PERSISTENT_TRANSACTION_TEST_H_

#include <gtest/gtest.h>
#include <memory>
#include "Exchange.h"

namespace tiny_mq {
class Exchange;
}

class PersistentTransactionTest : public ::testing::Test {
public:
    PersistentTransactionTest() = default;
    ~PersistentTransactionTest() override = default;
    
protected:
    void SetUp() override;
    void TearDown() override;
    
    std::unique_ptr<tiny_mq::Exchange> _exchange;
    
    static constexpr size_t batchSize = 10;
    static constexpr size_t batchCount = 3;
};

#endif // TINY_MQ_TESTS_PERSISTENT_TRANSACTION_TEST_H_