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
constexpr usize NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
// constexpr usize NUM_PAGES = 16;
constexpr usize REAP_RESERVE = (usize)(NUM_PAGES * 0.2);
// constexpr usize REAP_THRESHOLD = 4;
constexpr usize SWAP_SIZE = 2 * GB;
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

void set_permissions(uptr vaddr, u64 flags);
void clear_permissions(uptr vaddr, u64 flags);
bool pte_is_present(u64 pte);
bool pte_is_writable(u64 pte);
bool pte_is_accessed(u64 pte);
bool pte_is_dirty(u64 pte);
// Map a virtual page to a physical page
void map_gva(uptr gva, uptr gpa);
// Unmap a page from the guest page table
void unmap_gva(uptr gva);

struct SwapperStats {
  std::atomic<usize> total_faults = 0;
  std::atomic<usize> demand_zeros = 0;
  std::atomic<usize> swap_ins = 0;
  std::atomic<usize> swap_outs = 0;
  // evictions that skip write-back
  std::atomic<usize> clean_evictions = 0;
  // evictions that happened because
  // no free slot was find in the cache
  std::atomic<usize> reactive_evictions = 0;
  // evictions that happened because
  // of the background reaper
  std::atomic<usize> proactive_evictions = 0;
  std::atomic<usize> promotions = 0;
  std::atomic<usize> demotions = 0;
};

class Swapper {
public:
  Swapper(std::unique_ptr<Storage> storage);
  ~Swapper();

  // The main entry point called by VoliMem on a page fault
  void handle_fault(void *fault_addr, regstate_t *regstate);

  // Starts the background thread for LRU rebalancing
  void start_background_rebalancing();

  // Demotion: hot -> cold
  void demote_cold_pages();
  // Promotion: cold -> hot
  void promote_hot_pages();
  // Reap: proactively free up cold pages if below a reserve target
  void reap_cold_pages();

  void print_state(bool use_lock = true);

  void print_stats();

private:
  void swap_in_page(Page *page, uptr aligned_fault_vaddr);
  void swap_out_page(Page *page);

  // Returns the index of the slot where a page is located
  usize get_page_idx(Page *page);
  // Returns the GVA associated with a page
  uptr get_cache_gva(Page *page);

  /**
   * @brief Acquires a free physical page slot to service a page fault.
   *
   * This method is the core of the page replacement policy. It attempts to
   * find a free page using a tiered strategy:
   *
   * 1.  **Check `free_list_`:** The fastest path. If a pre-reaped page is
   *     available, it is returned immediately.
   *
   * 2.  **Evict from `inactive_list_`:** If no free pages are available, it
   *     selects a victim from the `inactive_list_`. This list holds pages
   *     that have not been accessed recently. The victim is chosen from the
   *     BACK of the list, which represents the page that has been "cold" for
   *     the longest time (a FIFO policy for cold pages).
   *
   * 3.  **Evict from `active_list_`:** In high-pressure scenarios where the
   *     `inactive_list_` is also empty, it is forced to steal a page from
   *     the `active_list_`. It chooses the page at the BACK, which is the
   *     Least Recently Used "hot" page.
   *
   * After a victim is chosen, it is swapped out (if dirty) and its physical
   * slot is returned for reuse.
   *
   *    [ New/Hot Pages ]           [ Recently Cold ]         [ Oldest/Coldest ]
   *          |                             |                         |
   *          v                             v                         v
   *  FRONT <--- [ P3, P2, P1 ] <--- BACK   FRONT <--- [ P9, P8, P7 ] <--- BACK
   *         active_list_                          inactive_list_
   *          |       ^                           |         ^
   *          |       |                           |         |
   *          +-------+                           +---------+
   *        Stay if                              Evicted if no
   *        accessed again                      free pages exist
   *
   * @note This function assumes the caller holds the pages_mutex_.
   */
  Page *acquire_page();

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

  // Map tracking the state of every faulted virtual address
  std::unordered_map<uptr, PageState> vaddr_state_map_;

  // Guards access to all cache metadata
  std::mutex pages_mutex_;

  std::thread rebalance_thread_;
  // how often the rebalance thread runs, recomputed at runtime
  u32 rebalance_interval_ms_ = 200;
  // shared between fault handler and the rebalance thread
  std::atomic<u32> evictions_in_cycle_ = 0;
  // how many cycles passed without the pressure of too many evictions
  u32 cycles_since_bad_event_ = 0;
  void adapt_rebalance_interval();
  // AIMD constants
  static constexpr u32 MIN_INTERVAL_MS = 20;  // most aggressive
  static constexpr u32 MAX_INTERVAL_MS = 500; // most relaxed
  static constexpr u32 ADDITIVE_INCREASE_MS = 10;
  static constexpr float MULTIPLICATIVE_DECREASE_FACTOR = 0.5;
  // react if >n sync evictions happen in one cycle
  static constexpr u8 PRESSURE_THRESHOLD = 3;
  // relax only after n good cycles in a row
  static constexpr u8 COOLDOWN_CYCLES = 6;

  SwapperStats stats_;
};
