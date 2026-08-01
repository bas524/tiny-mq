#ifndef TESTHELPER_H
#define TESTHELPER_H

#include <Poco/File.h>
#include <gtest/gtest.h>
#include <string>

#define CurrentTestName ::testing::UnitTest::GetInstance()->current_test_info()->name()

// Storage directory unique to the running test suite (fixture), e.g.
// "./tiny-mq-test-storage/ClientAckTest". Every fixture that constructs an
// Exchange/storage on disk must use its own directory so that --gtest_repeat
// and --gtest_shuffle runs never see another iteration's (or another suite's)
// replayed persistent state.
inline std::string CurrentTestSuiteStorageDir() {
  return std::string("./tiny-mq-test-storage/") +
         ::testing::UnitTest::GetInstance()->current_test_info()->test_suite_name();
}

// Recursively removes a test storage directory if present. Call from both
// SetUp (defensive, in case a previous run was killed mid-test) and TearDown.
inline void RemoveTestStorageDir(const std::string& path) {
  Poco::File f(path);
  if (f.exists()) {
    f.remove(true);
  }
}

#endif // TESTHELPER_H
