//
// Created by Alexander Bychuk on 17.09.2023.
//

#ifndef TINY_MQ_TESTS_LINEAR_STORAGE_TEST_H_
#define TINY_MQ_TESTS_LINEAR_STORAGE_TEST_H_

#include <gtest/gtest.h>
#include <Poco/Path.h>

class LinearStorageTest : public ::testing::Test {
  public:
  LinearStorageTest() = default;
  ~LinearStorageTest() override = default;
  
  protected:
  Poco::Path _basePath;
  void SetUp() override;
  void TearDown() override;
};


#endif // TINY_MQ_TESTS_LINEAR_STORAGE_TEST_H_
