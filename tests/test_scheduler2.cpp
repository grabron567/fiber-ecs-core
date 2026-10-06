#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

int test_deps()
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  
  JobCounter counter;
  std::atomic<int> a{0}, b{0}, c{0};
  
  scheduler.schedule([&]() {
    a.store(1);
  }, &counter);
  
  scheduler.schedule([&]() {
    b.store(a.load() + 1);
  }, &counter);
  
  scheduler.wait(&counter);
  
  if (a.load() != 1 || b.load() != 2) return 1;
  return 0;
}

int test_heavy()
{
  JobScheduler scheduler;
  scheduler.initialize();
  
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 50000;
  
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() {
      result.fetch_add(1, std::memory_order_relaxed);
    }, &counter);
  }
  
  scheduler.wait(&counter);
  return (result.load() == n && counter.count.load() == 0) ? 0 : 1;
}

int main()
{
  if (test_deps() != 0) return 1;
  if (test_heavy() != 0) return 2;
  return 0;
}
