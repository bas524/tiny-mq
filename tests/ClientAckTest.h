//
// CLIENT_ACKNOWLEDGE and INDIVIDUAL_ACKNOWLEDGE mode tests for tiny-mq
//

#ifndef TINY_MQ_TESTS_CLIENT_ACK_TEST_H_
#define TINY_MQ_TESTS_CLIENT_ACK_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class ClientAckTest : public ::testing::Test {
public:
    ClientAckTest() = default;
    ~ClientAckTest() override = default;

protected:
    void SetUp() override;
    void TearDown() override;

    std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_CLIENT_ACK_TEST_H_
