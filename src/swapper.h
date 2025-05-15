#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "volimem/mapper.h"

#define UNUSED(x) (void)(x)

// ANSI color codes
#define CLR_RESET "\033[0m"
#define CLR_DEBUG "\033[36m"
#define CLR_INFO "\033[32m"
#define CLR_WARN "\033[33m"
#define CLR_ERROR "\033[31m"
#define CLR_PANIC "\033[1;31m" // bright red

#define DEBUG(fmt, ...)                                                        \
  printf(CLR_DEBUG "[DEBUG] " fmt CLR_RESET "\n", ##__VA_ARGS__)
#define INFO(fmt, ...)                                                         \
  printf(CLR_INFO "[INFO]  " fmt CLR_RESET "\n", ##__VA_ARGS__)
#define WARN(fmt, ...)                                                         \
  printf(CLR_WARN "[WARN]  " fmt CLR_RESET "\n", ##__VA_ARGS__)
#define ERROR(fmt, ...)                                                        \
  printf(CLR_ERROR "[ERROR] " fmt CLR_RESET "\n", ##__VA_ARGS__)

#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, CLR_PANIC "[PANIC] %s:%d (%s): " fmt CLR_RESET "\n",       \
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

using u8 = uint8_t;
using uptr = uintptr_t;
using usize = size_t;

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Milliseconds = std::chrono::milliseconds;

constexpr usize KB = 1024;
constexpr usize MB = KB * KB;
constexpr usize GB = MB * KB;

constexpr usize PAGE_SIZE = 4 * KB;
constexpr usize CACHE_SIZE = 128 * MB;
// constexpr usize NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr usize NUM_PAGES = 2;
constexpr usize SWAP_SIZE = 1 * GB;
constexpr usize HEAP_SIZE = SWAP_SIZE;
constexpr uptr HEAP_START = 0xffff800000000000;

constexpr auto COLD_THRESHOLD = std::chrono::milliseconds(500);

enum class PageState : u8 {
  // slot is empty
  Free,
  // slot holds a page
  Mapped,
};

constexpr const char *page_state_to_str(PageState state) {
  switch (state) {
  case PageState::Free:
    return "Free";
  case PageState::Mapped:
    return "Mapped";
  default:
    return "Unknown";
  }
}

constexpr const char *bool_to_str(bool b) { return b ? "true" : "false"; }

struct Page {
  // Maps a virtual page from the heap to a cache slot
  uptr vaddr;
  PageState state;

  Page() : vaddr(0), state(PageState::Free) {}

  void print() const {
    printf("Page {\n  vaddr: 0x%lx,\n  state: %s\n}\n", vaddr,
           page_state_to_str(state));
  }
};

class Swapper {
public:
  virtual ~Swapper() = default;
  virtual void swap_in() = 0;
  virtual void swap_out() = 0;

protected:
  Swapper() = default; // prevent direct instantiation
};
