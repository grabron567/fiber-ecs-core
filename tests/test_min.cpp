#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

int main()
{
  JobScheduler scheduler;
  scheduler.initialize(1);
  JobCounter counter;
  std::atomic<int> done{0};
  scheduler.schedule([&]() { done.store(1); }, &counter);
  scheduler.wait(&counter);
  return (done.load() == 1 && counter.count.load() == 0) ? 0 : 1;
}
