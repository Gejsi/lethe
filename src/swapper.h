#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define UNUSED(x) (void)(x)
#define DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) printf("[WARNING] " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, "\x1b[31m[PANIC] %s:%d (%s): " fmt "\x1b[0m\n", __FILE__,  \
            __LINE__, __func__, ##__VA_ARGS__);                                \
    abort();                                                                   \
  } while (0)

#define ASSERT_EQ(val1, val2, msg)                                             \
  if ((val1) != (val2)) {                                                      \
    fprintf(stderr,                                                            \
            "\x1b[31mASSERTION FAILED (%s:%d): %s\n  Expected: %ld (0x%lx), "  \
            "Got: %ld (0x%lx)\x1b[0m\n",                                       \
            __FILE__, __LINE__, msg, (long)(val1), (uptr)(val1), (long)(val2), \
            (uptr)(val2));                                                     \
    abort();                                                                   \
  } else {                                                                     \
    printf("\x1b[32m[PASS] Assertion: %s\x1b[0m\n", msg);                      \
  }

using u8 = uint8_t;
using uptr = uintptr_t;
using usize = size_t;

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

const auto COLD_THRESHOLD = std::chrono::milliseconds(500);

class Swapper {
public:
  virtual ~Swapper() = default;
  virtual void swap_in() = 0;
  virtual void swap_out() = 0;

protected:
  Swapper() = default; // prevent direct instantiation
};

struct Page {
  // In the heap on the CPU node
  uptr vaddr;

  void print() const { printf("Page { vaddr: 0x%lx }\n", vaddr); }
};
