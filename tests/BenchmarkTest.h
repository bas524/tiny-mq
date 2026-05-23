#ifndef TINY_MQ_TESTS_BENCHMARKTEST_H_
#define TINY_MQ_TESTS_BENCHMARKTEST_H_

#include <benchmark/benchmark.h>
#include <memory>

namespace tiny_mq {
class Exchange;
}

class BenchmarkFixture : public benchmark::Fixture {
public:
    std::unique_ptr<tiny_mq::Exchange> exchange;
    void SetUp(::benchmark::State& state) override;
    void TearDown(::benchmark::State& state) override;
};

#endif  // TINY_MQ_TESTS_BENCHMARKTEST_H_
