#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

int main()
{
  JobScheduler scheduler;
  scheduler.initialize(4);
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 10000;
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() { result.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  if (result.load() != n || counter.count.load() != 0) return 1;
  return 0;
}
