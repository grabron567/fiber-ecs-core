#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

int main()
{
  JobScheduler scheduler;
  scheduler.initialize(4);
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 100;
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() { result.fetch_add(1); }, &counter);
  }
  scheduler.wait(&counter);
  return (result.load() == n && counter.count.load() == 0) ? 0 : 1;
}
