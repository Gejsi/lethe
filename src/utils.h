#pragma once

#include <chrono>

#include "types.h"

#define UNUSED(x) (void)(x)

// ANSI color codes
#define CLR_RESET "\033[0m"
#define CLR_DEBUG "\033[36m"
#define CLR_INFO "\033[32m"
#define CLR_WARN "\033[33m"
#define CLR_ERROR "\033[31m"
#define CLR_PANIC "\033[1;31m" // bright red

#ifdef USE_ASYNC_LOGGER
#include "logger.h"
#define LOG_IMPL(color, level, fmt, ...)                                       \
  AsyncLogger::instance().log(level, fmt, ##__VA_ARGS__)
#else
#define LOG_IMPL(color, level, fmt, ...)                                       \
  printf(color "[%s] " fmt CLR_RESET "\n", level, ##__VA_ARGS__)
#endif

#define DEBUG(fmt, ...) LOG_IMPL(CLR_DEBUG, "DEBUG", fmt, ##__VA_ARGS__)
#define INFO(fmt, ...) LOG_IMPL(CLR_INFO, "INFO", fmt, ##__VA_ARGS__)
#define WARN(fmt, ...) LOG_IMPL(CLR_WARN, "WARN", fmt, ##__VA_ARGS__)
#define ERROR(fmt, ...) LOG_IMPL(CLR_ERROR, "ERROR", fmt, ##__VA_ARGS__)

/*
#define DEBUG(fmt, ...) ((void)0)
#define INFO(fmt, ...) ((void)0)
#define WARN(fmt, ...) ((void)0)
#define ERROR(fmt, ...) ((void)0)
*/

#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, CLR_PANIC "[PANIC] %s:%d (%s): " fmt CLR_RESET "\n",       \
            __FILE__, __LINE__, __func__, ##__VA_ARGS__);                      \
    abort();                                                                   \
  } while (0)

#define UNREACHABLE(fmt, ...)                                                  \
  do {                                                                         \
    fprintf(stderr, CLR_PANIC "[UNREACHABLE] %s:%d (%s): " fmt CLR_RESET "\n", \
            __FILE__, __LINE__, __func__, ##__VA_ARGS__);                      \
    abort();                                                                   \
  } while (0)

// basically `assert` but doesn't flood
// with register errors when called within a volimem fault handler
#define ENSURE(cond, msg)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      snprintf(__VOLIMEM_ERROR_BUF, BUFSIZ,                                    \
               CLR_ERROR "Assertion failed in %s (%s:%u): %s" CLR_RESET "\n",  \
               __FILE__, __func__, __LINE__, msg);                             \
      zthrow(__VOLIMEM_ERROR_BUF);                                             \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(val1, val2, msg)                                             \
  if ((val1) != (val2)) {                                                      \
    fprintf(stderr,                                                            \
            CLR_ERROR                                                          \
            "ASSERTION FAILED (%s:%d): %s\n  Expected: %ld (0x%lx), "          \
            "Got: %ld (0x%lx)" CLR_RESET "\n",                                 \
            __FILE__, __LINE__, msg, (long)(val1), (uptr)(val1), (long)(val2), \
            (uptr)(val2));                                                     \
    abort();                                                                   \
  } else {                                                                     \
    printf(CLR_INFO "[PASS] Assertion: %s" CLR_RESET "\n", msg);               \
  }

constexpr usize KB = 1024;
constexpr usize MB = KB * KB;
constexpr usize GB = MB * KB;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;

constexpr const char *bool_to_str(bool b) noexcept {
  return b ? "true" : "false";
}

void sleep_ms(usize ms);
