#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>
#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

TEST_CASE("Job dependencies work")
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  JobCounter counter;
  std::atomic<int> a{0}, b{0};
  scheduler.schedule([&]() { a.store(1); }, &counter);
  scheduler.schedule([&]() { b.store(a.load() + 1); }, &counter);
  scheduler.wait(&counter);
  REQUIRE(a.load() == 1);
  REQUIRE(b.load() == 2);
  REQUIRE(counter.count.load() == 0);
}

TEST_CASE("Concurrent work stealing")
{
  JobScheduler scheduler;
  scheduler.initialize();
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 10000;
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() { result.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  REQUIRE(result.load() == n);
  REQUIRE(counter.count.load() == 0);
}
