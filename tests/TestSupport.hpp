#pragma once

// Thin assertion shim so the same test bodies compile against Catch2 v3,
// GoogleTest, or the built-in fallback harness selected by tests/CMakeLists.txt.

#if defined(FIBERECS_TEST_CATCH2)
#include <catch2/catch_test_macros.hpp>

#define FIBERECS_TEST_CASE(name) TEST_CASE(name)
#define FIBERECS_CHECK(expr) REQUIRE(expr)
#define FIBERECS_CHECK_EQ(a, b) REQUIRE((a) == (b))
#define FIBERECS_REQUIRE_THROWS(expr) REQUIRE_THROWS(expr)

#elif defined(FIBERECS_TEST_GTEST)
#include <gtest/gtest.h>

#define FIBERECS_CONCAT_INNER(a, b) a##b
#define FIBERECS_CONCAT(a, b) FIBERECS_CONCAT_INNER(a, b)
#define FIBERECS_TEST_CASE(name) TEST(FIBERECS_CONCAT(Suite_, __LINE__), name)
#define FIBERECS_CHECK(expr) EXPECT_TRUE(expr)
#define FIBERECS_CHECK_EQ(a, b) EXPECT_EQ((a), (b))
#define FIBERECS_REQUIRE_THROWS(expr) EXPECT_THROW((void)(expr), std::exception)

#else

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace fiberecs_test
{
struct Case
{
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& registry()
{
  static std::vector<Case> cases;
  return cases;
}

inline int& failures()
{
  static int count = 0;
  return count;
}

struct Registrar
{
  Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void report(bool ok, const char* expr, const char* file, int line)
{
  if (!ok)
  {
    std::printf("  [FAIL] %s:%d: %s\n", file, line, expr);
    ++failures();
  }
  std::fflush(stdout);
}

inline int run_all(const char* filter = nullptr)
{
  std::size_t ran = 0;
  for (const Case& c : registry())
  {
    if (filter != nullptr && std::string(c.name).find(filter) == std::string::npos)
    {
      continue;
    }
    ++ran;
    const int before = failures();
    std::printf("  running %s\n", c.name);
    try
    {
      c.fn();
    }
    catch (const std::exception& e)
    {
      std::printf("  [FAIL] %s threw: %s\n", c.name, e.what());
      ++failures();
    }
    catch (...)
    {
      std::printf("  [FAIL] %s threw an unknown exception\n", c.name);
      ++failures();
    }
    if (failures() == before)
    {
      std::printf("  [ ok ] %s\n", c.name);
    }
    std::fflush(stdout);
  }
  if (failures() == 0)
  {
    std::printf("all %zu test case(s) passed\n", ran);
    return 0;
  }
  std::printf("%d check(s) failed\n", failures());
  return 1;
}
} // namespace fiberecs_test

#define FIBERECS_CONCAT_INNER(a, b) a##b
#define FIBERECS_CONCAT(a, b) FIBERECS_CONCAT_INNER(a, b)

#define FIBERECS_TEST_CASE(name)                                                    \
  static void FIBERECS_CONCAT(fiberecs_test_body_, __LINE__)();                     \
  static ::fiberecs_test::Registrar FIBERECS_CONCAT(fiberecs_test_reg_, __LINE__)(   \
      name, &FIBERECS_CONCAT(fiberecs_test_body_, __LINE__));                       \
  static void FIBERECS_CONCAT(fiberecs_test_body_, __LINE__)()

#define FIBERECS_CHECK(expr)                                                        \
  ::fiberecs_test::report(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define FIBERECS_CHECK_EQ(a, b)                                                     \
  ::fiberecs_test::report((a) == (b), #a " == " #b, __FILE__, __LINE__)

#define FIBERECS_REQUIRE_THROWS(expr)                                               \
  do                                                                                \
  {                                                                                 \
    bool threw = false;                                                              \
    try                                                                             \
    {                                                                               \
      (void)(expr);                                                                 \
    }                                                                               \
    catch (...)                                                                     \
    {                                                                               \
      threw = true;                                                                 \
    }                                                                               \
    ::fiberecs_test::report(threw, "throws: " #expr, __FILE__, __LINE__);           \
  } while (false)

#endif // framework selection