// A tiny test helper (no library needed).
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

static int g_failed = 0, g_checked = 0;

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    ++g_checked;                                                                       \
    if (!(cond)) {                                                                     \
      ++g_failed;                                                                      \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                    \
    }                                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                 \
  do {                                                                                 \
    ++g_checked;                                                                       \
    const auto va = (a);                                                               \
    const auto vb = (b);                                                               \
    if (!(va == vb)) {                                                                 \
      ++g_failed;                                                                      \
      std::printf("  FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);             \
    }                                                                                  \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
  do {                                                                                 \
    ++g_checked;                                                                       \
    if (std::fabs(static_cast<double>(a) - static_cast<double>(b)) > (tol)) {          \
      ++g_failed;                                                                      \
      std::printf("  FAIL %s:%d  %s ~ %s  (%g vs %g)\n", __FILE__, __LINE__, #a, #b,   \
                  static_cast<double>(a), static_cast<double>(b));                     \
    }                                                                                  \
  } while (0)

#define TEST_MAIN_END()                                                                \
  std::printf("%d checks, %d failed\n", g_checked, g_failed);                          \
  return g_failed == 0 ? 0 : 1
