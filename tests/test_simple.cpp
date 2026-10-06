#include "../core/LinearArena.hpp"
#include "../core/RingArena.hpp"
#include "../job/WorkStealingQueue.hpp"
#include "../job/JobScheduler.hpp"
#include <atomic>
#include <iostream>

using namespace fiberecs;

int main()
{
  LinearArena arena(1024);
  void* p1 = arena.allocate(32, 64);
  void* p2 = arena.allocate(64, 64);
  (void)p1; (void)p2;
  if (arena.used() == 0) return 1;
  arena.reset();
  if (arena.used() != 0) return 1;
  RingArena r(1024);
  void* rp = r.allocate(32);
  (void)rp;
  
  WorkStealingQueue<int> queue(10);
  int x = 5;
  queue.push(&x);
  int* popped = queue.pop();
  (void)popped;
  
  std::cout << "All tests passed" << std::endl;
  return 0;
}
