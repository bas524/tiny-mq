// JMSPriority ordering tests for tiny-mq (spec 45 — priority ordering on dequeue).
//
// Test plan:
//   (a) Interleaved send of p=0 and p=9 — receiver sees all p=9 before p=0.
//   (b) Uniform-priority workload — ordering remains FIFO; adjacent-priority
//       interleave (p=4/p=5) additionally verifies intra-band FIFO.
//   (c) Restart — priority banding is preserved across Exchange restarts.

#ifndef TINY_MQ_TESTS_PRIORITY_ORDERING_TEST_H_
#define TINY_MQ_TESTS_PRIORITY_ORDERING_TEST_H_

#include <gtest/gtest.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class PriorityOrderingTest : public ::testing::Test {
 public:
  PriorityOrderingTest() = default;
  ~PriorityOrderingTest() override = default;

 protected:
  void SetUp() override;
  void TearDown() override;

  std::unique_ptr<tiny_mq::Exchange> _exchange;
};

#endif  // TINY_MQ_TESTS_PRIORITY_ORDERING_TEST_H_
