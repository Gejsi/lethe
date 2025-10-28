#pragma once

#include <cstdio>
#include <cstdlib>

#include "utils.h"

constexpr usize PAGE_SIZE = 4 * KB;
constexpr usize CACHE_SIZE = 128 * MB;
// constexpr usize NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr usize NUM_PAGES = 3;
constexpr usize SWAP_SIZE = 1 * GB;
constexpr usize HEAP_SIZE = SWAP_SIZE;
constexpr uptr HEAP_START = 0xffff800000000000;

enum class PageState : u8 { Unmapped, Mapped, RemotelyMapped };

constexpr const char *page_state_to_str(PageState state) {
  switch (state) {
  case PageState::Unmapped:
    return "Unmapped";
  case PageState::Mapped:
    return "Mapped";
  case PageState::RemotelyMapped:
    return "RemotelyMapped";
  default:
    return "Unknown";
  }
}

struct Page {
  // Maps a virtual page from the heap to a cache slot
  uptr vaddr;

  Page() : vaddr(0) {}

  void reset() { *this = Page{}; }

  void print(bool inline_output = true) const {
    if (inline_output) {
      printf("Page { vaddr: 0x%lx }\n", vaddr);
    } else {
      printf("Page {\n");
      printf("  vaddr: 0x%lx,\n", vaddr);
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

void set_permissions(uptr vaddr, u64 flags, bool flush = true);
void clear_permissions(uptr vaddr, u64 flags);
bool pte_is_present(u64 pte);
bool pte_is_writable(u64 pte);
bool pte_is_accessed(u64 pte);
bool pte_is_dirty(u64 pte);
// Map a virtual page to a physical page
void map(uptr gva, uptr gpa);
// Unmap a page from the guest page table
void unmap(uptr gva);
