#include "TestSupport.hpp"

#if defined(FIBERECS_TEST_BUILTIN)
int main(int argc, char** argv)
{
  // An optional substring filter keeps bisecting a failing build cheap.
  return ::fiberecs_test::run_all(argc > 1 ? argv[1] : nullptr);
}
#endif
