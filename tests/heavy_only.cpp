#include "../job/JobScheduler.hpp"
#include <atomic>
using namespace fiberecs;
int main() {
  JobScheduler s; s.initialize();
  JobCounter c; std::atomic<int> r{0}; const int n=50000;
  for(int i=0;i<n;i++) s.schedule([&r](){r.fetch_add(1,std::memory_order_relaxed);},&c);
  s.wait(&c);
  return (r.load()==n && c.count.load()==0)?0:1;
}
