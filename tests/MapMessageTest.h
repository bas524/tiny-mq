//
// Created by Alexander Bychuk on 26.03.2022.
//

#ifndef TINY_MQ_TESTS_MAPMESSAGETEST_H_
#define TINY_MQ_TESTS_MAPMESSAGETEST_H_

#include <gtest/gtest.h>
#include "Exchange.h"

class MapMessageTest : public ::testing::Test {
 public:
  MapMessageTest() = default;
  ~MapMessageTest() override = default;

 protected:
  std::unique_ptr<tiny_mq::Exchange> _exchange;
  void SetUp() override;
  void TearDown() override;
};

#endif  // TINY_MQ_TESTS_MAPMESSAGETEST_H_
