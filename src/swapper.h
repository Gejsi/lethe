#pragma once

#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <vector>
#include <volimem/idt.h>

#include "storage/storage.h"
#include "utils.h"

constexpr usize SWAP_SIZE = 10 * GB;
constexpr uptr HEAP_START = 0xffff800000000000;

constexpr bool pte_is_present(u64 pte);
constexpr bool pte_is_writable(u64 pte);
constexpr bool pte_is_accessed(u64 pte);
constexpr bool pte_is_dirty(u64 pte);

void set_permissions(uptr vaddr, u64 flags);
void clear_permissions(uptr vaddr, u64 flags);
// Map a virtual page to a physical page
void map_gva(uptr gva, uptr gpa);
// Unmap a page from the guest page table
void unmap_gva(uptr gva);

struct SwapperConfig {
  usize cache_size = 3600 * MB;

  usize num_pages;
  usize heap_size;
  usize num_heap_pages;

  SwapperConfig() = default;

  // move constructor: derives values when moved
  SwapperConfig(SwapperConfig &&other) noexcept
      : cache_size(other.cache_size), num_pages(cache_size / PAGE_SIZE),
        heap_size(cache_size + SWAP_SIZE),
        num_heap_pages(heap_size / PAGE_SIZE) {}

  SwapperConfig(const SwapperConfig &) = delete;
  SwapperConfig &operator=(const SwapperConfig &) = delete;
};

enum class PageState : u8 {
  // Cache slot is empty: access triggers a demand-zero allocation.
  // NOTE: keep this as the first entry of the enum
  Unmapped,

  // Page is in cache but has NO copy on the storage:
  // basically, a newly allocated demand-zero page.
  // If evicted while clean, it can be fully discared
  FreshlyMapped,

  // Page is in cache and HAS a valid copy on the storage:
  // basically, a page retrieved from the swap area.
  // If evicted while clean, the write-back can be skipped
  Mapped,

  // Page is in storage: access triggers a swap-in to bring it in cache
  RemotelyMapped
};

constexpr const char *page_state_to_str(PageState state) {
  switch (state) {
  case PageState::Unmapped:
    return "Unmapped";
  case PageState::FreshlyMapped:
    return "FreshlyMapped";
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

  void reset() { vaddr = 0; }

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

struct SwapperStats {
  usize total_faults = 0;
  usize demand_zeros = 0;
  usize swap_ins = 0;
  // evictions of dirty pages
  usize swap_outs = 0;
  // evictions of Mapped clean pages
  usize skipped_writes = 0;
  // evictions of FreshlyMapped clean pages
  usize discards = 0;
  // evictions from direct_reclaim
  usize reclaim_evictions = 0;
  usize promotions = 0;
  usize demotions = 0;
};

class Swapper {
public:
  Swapper(SwapperConfig &&swapper_config, std::unique_ptr<Storage> storage);
  ~Swapper();

  // The main entry point called by VoliMem on a page fault
  void handle_fault(void *fault_addr, regstate_t *regstate);

  void print_stats();

  SwapperConfig config;

private:
  void swap_in_page(Page *page, uptr aligned_fault_vaddr);
  void swap_out_page(Page *page);

  // Returns the index of the cache slot where a page is located
  usize get_page_idx(Page *page);
  // Returns the GVA associated with a page
  uptr get_cache_gva(Page *page);
  // Returns the index of the heap slot where a page is located
  usize get_heap_idx(uptr vaddr);

  // Synchronous direct reclaim: evict pages until nr_to_reclaim are freed
  void direct_reclaim(usize nr_to_reclaim);
  // Scan inactive tail, evict cold pages, promote hot ones back to active
  usize shrink_inactive_list(usize nr_to_reclaim);
  // Scan active tail, demote cold pages to inactive, second-chance hot ones
  void shrink_active_list();

  static constexpr usize RECLAIM_BATCH = 64;

  std::unique_ptr<Storage> storage_; // The abstract storage backend
  void *cache_base_addr_;            // The base address of the cache
  // Fixed-size array describing where a page is located in the cache
  std::unique_ptr<Page[]> pages_;
  // Free page slots (simple stack: push_back/pop_back)
  std::vector<Page *> free_pages_;
  // Two-list LRU: pages start on inactive, get promoted to active on re-access
  std::list<Page *> active_pages_;
  std::list<Page *> inactive_pages_;

  // Track the state of all pages, both on the cache and on the swap area
  std::unique_ptr<std::atomic<PageState>[]> page_states_;

  SwapperStats stats_;
};
