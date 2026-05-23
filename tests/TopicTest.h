//
// Topic (pub/sub) tests for tiny-mq
//

#ifndef TINY_MQ_TESTS_TOPIC_TEST_H_
#define TINY_MQ_TESTS_TOPIC_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class TopicTest : public ::testing::Test {
public:
    TopicTest() = default;
    ~TopicTest() override = default;

protected:
    void SetUp() override;
    void TearDown() override;

    std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_TOPIC_TEST_H_
