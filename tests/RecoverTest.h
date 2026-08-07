//
// Session.recover() tests for tiny-mq (JMS 2.0 § 8.4.8, spec 23)
//

#ifndef TINY_MQ_TESTS_RECOVER_TEST_H_
#define TINY_MQ_TESTS_RECOVER_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class RecoverTest : public ::testing::Test {
public:
    RecoverTest() = default;
    ~RecoverTest() override = default;

protected:
    void SetUp() override;
    void TearDown() override;

    std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_RECOVER_TEST_H_
