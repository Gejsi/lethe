#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define UNUSED(x) (void)(x)
#define DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, "\x1b[31m[PANIC] %s:%d (%s): " fmt "\x1b[0m\n", __FILE__,  \
            __LINE__, __func__, ##__VA_ARGS__);                                \
    abort();                                                                   \
  } while (0)

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

class Swapper {
public:
  virtual ~Swapper() = default;
  virtual void swap_in(void *cache_slot_phys_addr,
                       uintptr_t faulting_page_vaddr) = 0;
  virtual void swap_out(void *cache_slot_phys_addr,
                        uintptr_t victim_page_vaddr) = 0;

protected:
  Swapper() = default; // prevent direct instantiation
};

struct Page {
  // In the heap on the CPU node
  uptr vaddr;

  void print() const { printf("Page { vaddr: 0x%lx }\n", vaddr); }
};
