#include "../job/JobScheduler.hpp"
#include <atomic>
#include <iostream>

using namespace fiberecs;

int test_deps()
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  JobCounter counter;
  std::atomic<int> a{0}, b{0};
  scheduler.schedule([&]() { a.store(1); }, &counter);
  scheduler.schedule([&]() { b.store(a.load() + 1); }, &counter);
  scheduler.wait(&counter);
  std::cout << a.load() << " " << b.load() << " cnt=" << counter.count.load() << std::endl;
  if (a.load() != 1 || b.load() != 2) return 1;
  return 0;
}

int main() { return test_deps(); }
