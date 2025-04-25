#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define UNUSED(x) (void)(x)

#define DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define WARN(fmt, ...) printf("[WARN] " fmt "\n", ##__VA_ARGS__)
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
constexpr usize NUM_PAGES = 1;
constexpr usize SWAP_SIZE = 1 * GB;
constexpr usize HEAP_SIZE = SWAP_SIZE;
constexpr uptr HEAP_START = 0xffff800000000000;

constexpr auto COLD_THRESHOLD = std::chrono::milliseconds(500);

enum class PageState : u8 {
  // slot is empty
  Free,
  // slot holds an accessible page
  Mapped,
  // slot holds an inaccessible page
  Probed
};

constexpr const char *page_state_to_str(PageState state) {
  switch (state) {
  case PageState::Free:
    return "Free";
  case PageState::Mapped:
    return "Mapped";
  case PageState::Probed:
    return "Probed";
  default:
    return "Unknown";
  }
}

constexpr const char *bool_to_str(bool b) { return b ? "true" : "false"; }

struct Page {
  // Maps a virtual page from the heap to a cache slot
  uptr vaddr;
  std::chrono::steady_clock::time_point last_scan;
  PageState state;

  Page() : vaddr(0), state(PageState::Free), last_scan() {}

  void print() const {
    auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       last_scan.time_since_epoch())
                       .count();

    // auto cit = std::chrono::duration_cast<std::chrono::milliseconds>(
    //                last_fault - last_scan)
    //                .count();

    printf("Page {\n  vaddr: 0x%lx,\n  state: %s,\n  last_scan:  %ld ms,\n  "
           "last_fault: %ld ms,\n  cit: %ld\n}\n",
           vaddr, page_state_to_str(state), scan_ms, 0, 0);
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
