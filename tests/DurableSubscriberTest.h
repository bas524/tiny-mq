// Durable subscriber tests for tiny-mq (topic-family only)

#ifndef TINY_MQ_TESTS_DURABLE_SUBSCRIBER_TEST_H_
#define TINY_MQ_TESTS_DURABLE_SUBSCRIBER_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class DurableSubscriberTest : public ::testing::Test {
public:
    DurableSubscriberTest() = default;
    ~DurableSubscriberTest() override = default;

protected:
    void SetUp() override;
    void TearDown() override;

    std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_DURABLE_SUBSCRIBER_TEST_H_
