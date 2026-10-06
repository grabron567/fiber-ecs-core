# include/
#
# Header-only archetypal ECS. The target exists so test and benchmark
# executables can link a single library and inherit both the include root
# (include/ for ecs/..., project root for core/..., job/...) and the job
# system implementation.

add_library(fiberecs_ecs INTERFACE)
target_include_directories(fiberecs_ecs INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(fiberecs_ecs INTERFACE fiberecs_job)
