#pragma once

#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <vector>
#include <volimem/idt.h>

#include "concurrentqueue.h"
#include "storage/storage.h"
#include "utils.h"

constexpr usize PAGE_SIZE = 4 * KB;
constexpr usize CACHE_SIZE = 112 * MB;
constexpr usize NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr usize SWAP_SIZE = 256 * MB;
constexpr usize HEAP_SIZE = CACHE_SIZE + SWAP_SIZE;
constexpr uptr HEAP_START = 0xffff800000000000;
constexpr usize NUM_HEAP_PAGES = HEAP_SIZE / PAGE_SIZE;
constexpr usize NUM_SHARDS = 256;
constexpr usize REAP_RESERVE = (usize)(NUM_PAGES * 0.2);
constexpr usize SHARD_REAP_RESERVE = REAP_RESERVE / NUM_SHARDS;

void set_permissions(uptr vaddr, u64 flags);
void clear_permissions(uptr vaddr, u64 flags);
constexpr bool pte_is_present(u64 pte);
constexpr bool pte_is_writable(u64 pte);
constexpr bool pte_is_accessed(u64 pte);
constexpr bool pte_is_dirty(u64 pte);
// Map a virtual page to a physical page
void map_gva(uptr gva, uptr gpa);
// Unmap a page from the guest page table
void unmap_gva(uptr gva);

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

struct alignas(64) Shard {
  std::mutex mutex;

  // Lists used to organize the pages with a LRU policy
  std::list<Page *> active_pages;
  std::list<Page *> inactive_pages;

  // save the oldest page (back of inactive list).
  // 0 = Empty, UINT64_MAX = Newest.
  std::atomic<u64> oldest_age{0};
};

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
  std::atomic<usize> rebalancer_skips = 0;
};

class Swapper {
public:
  Swapper(std::unique_ptr<Storage> storage);
  ~Swapper();

  // The main entry point called by VoliMem on a page fault
  void handle_fault(void *fault_addr, regstate_t *regstate);

  // Starts the background thread for LRU rebalancing
  void start_background_rebalancing();
  // Stops the background thread for LRU rebalancing
  void stop_background_rebalancing();

  // Demotion: hot -> cold
  void demote_cold_pages(Shard &shard);
  // Promotion: cold -> hot
  void promote_hot_pages(Shard &shard);
  // Reap: proactively reclaim cold pages if below a reserve target
  void reap_cold_pages(Shard &shard);

  void print_state();

  void print_stats();

private:
  // TODO: improve error handling of these two methods
  void swap_in_page(Page *page, uptr aligned_fault_vaddr);
  void swap_out_page(Page *page);

  // Returns the index of the cache slot where a page is located
  usize get_page_idx(Page *page);
  // Returns the GVA associated with a page
  uptr get_cache_gva(Page *page);
  // Returns the index of the heap slot where a page is located
  usize get_heap_idx(uptr vaddr);
  // Returns the index of the shard where a page is handled
  usize get_shard_idx(uptr vaddr);

  // Acquires a free physical page slot to service a page fault.
  Page *acquire_page(Shard &shard);

  std::unique_ptr<Storage> storage_; // The abstract storage backend
  void *cache_base_addr_;            // The base address of the cache
  // Fixed-size array describing where a faulting address
  // is located in the physical cache
  std::unique_ptr<Page[]> pages_;
  // List of unmapped pages in the cache
  // TODO: implement my own simpler lock-free queue
  moodycamel::ConcurrentQueue<Page *> free_pages_queue_;

  std::vector<Shard> shards_;

  // Track the state of all pages, both on the cache and on the swap area
  std::unique_ptr<std::atomic<PageState>[]> page_states_;

  std::thread rebalancer_;
  std::atomic<bool> rebalancer_running_{true};
  // how often the rebalance thread runs, recomputed at runtime
  u32 rebalance_interval_ms_ = 200;
  // shared between fault handler and the rebalance thread
  std::atomic<u32> evictions_in_cycle_ = 0;
  // how many cycles passed without the pressure of too many evictions
  u32 stable_cycles_ = 0;
  void adapt_rebalance_interval();
  // AIMD constants
  static constexpr u32 MIN_INTERVAL_MS = 20;   // most aggressive
  static constexpr u32 MAX_INTERVAL_MS = 1000; // most relaxed
  static constexpr u32 ADDITIVE_INCREASE_MS = 10;
  static constexpr float MULTIPLICATIVE_DECREASE_FACTOR = 0.5;
  // react if >n sync evictions happen in one cycle
  static constexpr u8 PRESSURE_THRESHOLD = 3;
  // relax only after n good cycles in a row
  static constexpr u8 COOLDOWN_CYCLES = 6;

  SwapperStats stats_;
};
