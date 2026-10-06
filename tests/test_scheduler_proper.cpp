#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

bool test_dependencies()
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  JobCounter counter;
  std::atomic<int> a{0}, b{0}, c{0};
  scheduler.schedule([&]() { a.store(1); }, &counter);
  scheduler.schedule([&]() { b.store(a.load() + 1); c.store(b.load() + 1); }, &counter);
  scheduler.wait(&counter);
  return (a.load() == 1 && b.load() == 2 && c.load() == 3 && counter.count.load() == 0);
}

bool test_concurrent_heavy()
{
  JobScheduler scheduler;
  scheduler.initialize(4);
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 5000;
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() { result.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  return (result.load() == n && counter.count.load() == 0);
}

int main()
{
  if (!test_dependencies()) return 1;
  if (!test_concurrent_heavy()) return 2;
  return 0;
}
