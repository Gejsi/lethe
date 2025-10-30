#pragma once

#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <thread>
#include <unordered_map>
#include <volimem/idt.h>

#include "storage/storage.h"
#include "utils.h"

constexpr usize PAGE_SIZE = 4 * KB;
constexpr usize CACHE_SIZE = 128 * MB;
// constexpr usize NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr usize NUM_PAGES = 1;
constexpr usize REAP_THRESHOLD = 2;
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

void set_permissions(uptr vaddr, u64 flags, bool flush = true);
void clear_permissions(uptr vaddr, u64 flags);
bool pte_is_present(u64 pte);
bool pte_is_writable(u64 pte);
bool pte_is_accessed(u64 pte);
bool pte_is_dirty(u64 pte);
// Map a virtual page to a physical page
void map_gva(uptr gva, uptr gpa);
// Unmap a page from the guest page table
void unmap_gva(uptr gva);

class Swapper {
public:
  Swapper(std::unique_ptr<Storage> storage);
  ~Swapper();

  // The main entry point called by VoliMem on a page fault
  void handle_fault(void *fault_addr, regstate_t *regstate);

  // Starts the background thread for rebalancing
  void start_background_rebalancing();

  void print_state(const char *caller_name);

  std::thread rebalance_thread_;

private:
  void rebalance_lists();
  void swap_in_page(Page *page, uptr aligned_fault_vaddr);
  void swap_out_page(Page *page);
  usize get_page_idx(Page *page);
  uptr get_cache_gva(Page *page);

  std::unique_ptr<Storage> storage_; // The abstract storage backend
  void *cache_base_addr_;            // The base address of the cache
  // Fixed-size array describing where a faulting address
  // is located in the physical cache
  std::unique_ptr<Page[]> pages_;
  // Lists used to organize the pages based on their
  // usage (hot, cold, or free) for the LRU algorithm
  std::list<Page *> active_pages_;
  std::list<Page *> inactive_pages_;
  std::list<Page *> free_pages_;

  // Map tracking the state of every virtual page
  std::unordered_map<uptr, PageState> state_map_;

  // Guards access to all cache metadata
  std::mutex pages_mutex_;
};
