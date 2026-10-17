/*
 * libheif-hwdec - hardware HEVC and AV1 decoder plugin for libheif
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Minimal check macro for the unit tests: reports each failed condition with its location and
 * keeps going, so one run shows every failure.
 */

#ifndef HWDEC_TEST_UTIL_H
#define HWDEC_TEST_UTIL_H

#include <cstdio>

inline int& test_failures()
{
  static int failures = 0;
  return failures;
}

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      test_failures()++;                                                        \
    }                                                                           \
  } while (0)

inline int test_result()
{
  if (test_failures()) {
    fprintf(stderr, "%d check(s) failed\n", test_failures());
    return 1;
  }
  printf("all checks passed\n");
  return 0;
}

#endif
