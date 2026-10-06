#include "../job/JobScheduler.hpp"
#include <atomic>
#include <vector>

using namespace fiberecs;

int main()
{
  // Test dependencies
  {
    JobScheduler scheduler;
    scheduler.initialize(2);
    JobCounter counter;
    std::atomic<int> a{0}, b{0};
    scheduler.schedule([&]() { a.store(1); }, &counter);
    scheduler.schedule([&]() { b.store(a.load() + 5); }, &counter);
    scheduler.wait(&counter);
    if (a.load() != 1 || b.load() != 6 || counter.count.load() != 0) return 1;
  }
  // Test parent-child completion
  {
    JobScheduler scheduler;
    scheduler.initialize(2);
    JobCounter counter;
    std::atomic<int> result{0};
    const int n = 1000;
    for (int i = 0; i < n; ++i)
    {
      scheduler.schedule([&result, &scheduler, &counter]() {
        result.fetch_add(1);
        scheduler.schedule([&result]() { result.fetch_add(1); }, &counter);
      }, &counter);
    }
    scheduler.wait(&counter);
    if (result.load() != 2000 || counter.count.load() != 0) return 2;
  }
  // Test concurrent work-stealing under heavy contention
  {
    JobScheduler scheduler;
    scheduler.initialize();
    JobCounter counter;
    std::atomic<int> result{0};
    const int n = 50000;
    for (int i = 0; i < n; ++i)
    {
      scheduler.schedule([&result]() { result.fetch_add(1, std::memory_order_relaxed); }, &counter);
    }
    scheduler.wait(&counter);
    if (result.load() != n || counter.count.load() != 0) return 3;
  }
  return 0;
}
