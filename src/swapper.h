#pragma once

#include <cstdio>
#include <cstdlib>

#include "utils.h"

constexpr usize PAGE_SIZE = 4 * KB;
constexpr usize CACHE_SIZE = 128 * MB;
// constexpr usize NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr usize NUM_PAGES = 2;
constexpr usize SWAP_SIZE = 1 * GB;
constexpr usize HEAP_SIZE = SWAP_SIZE;
constexpr uptr HEAP_START = 0xffff800000000000;

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

struct Page {
  // Maps a virtual page from the heap to a cache slot
  uptr vaddr;
  PageState state;

  Page() : vaddr(0), state(PageState::Free) {}

  void print(bool inline_output = true) const {
    auto state_str = page_state_to_str(state);

    if (inline_output) {
      printf("Page { vaddr: 0x%lx, state: %s }\n", vaddr, state_str);
    } else {
      printf("Page {\n");
      printf("  vaddr: 0x%lx,\n", vaddr);
      printf("  state: %s\n", state_str);
      printf("}\n");
    }
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
