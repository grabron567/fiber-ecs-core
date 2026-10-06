# job: lock-free work-stealing queues + fiber job scheduler
add_library(fiberecs_job
  WorkStealingQueue.hpp
  JobScheduler.hpp
  JobScheduler.cpp)

target_link_libraries(fiberecs_job PUBLIC fiberecs_core fiberecs_options)