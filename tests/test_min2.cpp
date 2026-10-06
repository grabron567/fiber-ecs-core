#include "../job/JobScheduler.hpp"
#include <atomic>

using namespace fiberecs;

int main()
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  JobCounter counter;
  std::atomic<int> done{0}, done2{0};
  scheduler.schedule([&]() { done.store(1); }, &counter);
  scheduler.schedule([&]() { done2.store(1); }, &counter);
  scheduler.wait(&counter);
  return (done.load()==1 && done2.load()==1 && counter.count.load()==0) ? 0 : 1;
}
