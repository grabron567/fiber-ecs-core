#include "../job/JobScheduler.hpp"
#include <atomic>
#include <chrono>
#include <iostream>

using namespace fiberecs;

int main()
{
  JobScheduler scheduler;
  scheduler.initialize(2);
  JobCounter counter;
  std::atomic<int> result{0};
  const int n = 5000;
  auto start = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < n; ++i)
  {
    scheduler.schedule([&result]() { result.fetch_add(1, std::memory_order_relaxed); }, &counter);
  }
  scheduler.wait(&counter);
  auto end = std::chrono::high_resolution_clock::now();
  std::cout << std::chrono::duration_cast<std::chrono::microseconds>(end-start).count() << std::endl;
  if (result.load() != n || counter.count.load() == 0) return 0; // counter should be 0
  return (result.load() == n && counter.count.load() == 0) ? 0 : 1;
}
