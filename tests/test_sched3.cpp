#include "../job/JobScheduler.hpp"
#include <atomic>
#include <iostream>

using namespace fiberecs;

int main()
{
  JobScheduler scheduler;
  scheduler.initialize(4);
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 50000;
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() { result.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  std::cout << result.load() << " " << counter.count.load() << std::endl;
  return (result.load() == n && counter.count.load() == 0) ? 0 : 1;
}
