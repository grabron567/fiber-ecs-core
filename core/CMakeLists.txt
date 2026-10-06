# core: memory arenas + fiber context switching
add_library(fiberecs_core
  MemoryUtils.hpp
  LinearArena.hpp
  LinearArena.cpp
  BlockArena.hpp
  BlockArena.cpp
  RingArena.hpp
  RingArena.cpp
  Fiber.hpp
  Fiber.cpp)

if(FIBERECS_HAS_ASM_FIBER)
  target_sources(fiberecs_core PRIVATE fiber_context.S)
  target_compile_definitions(fiberecs_core PUBLIC FIBERECS_ASM_FIBER=1)
endif()

target_include_directories(fiberecs_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(fiberecs_core PUBLIC fiberecs_options)

set_target_properties(fiberecs_core PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)