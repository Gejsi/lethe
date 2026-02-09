#pragma once

#include <cstdio>
#include <cstdlib>
#include <list>
#include <memory>
#include <volimem/idt.h>

#include "concurrentqueue.h"
#include "storage/storage.h"
#include "utils.h"

constexpr usize SWAP_SIZE = 512 * MB;
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
  usize cache_size = 64 * MB;
  usize num_shards = 64;
  bool rebalancer_disabled = true;

  usize num_pages;
  usize heap_size;
  usize num_heap_pages;
  usize reap_reserve;
  usize shard_reap_reserve;

  // Rebalancer AIMD Configuration
  // u32 min_interval_ms;
  // u32 max_interval_ms;
  // u32 additive_increase_ms;
  // float multiplicative_decrease_factor;
  // u8 pressure_threshold;
  // u8 cooldown_cycles;

  SwapperConfig() = default;

  // move constructor: derives values when moved
  SwapperConfig(SwapperConfig &&other) noexcept
      : cache_size(other.cache_size), num_shards(other.num_shards),
        rebalancer_disabled(other.rebalancer_disabled),
        num_pages(cache_size / PAGE_SIZE), heap_size(cache_size + SWAP_SIZE),
        num_heap_pages(heap_size / PAGE_SIZE),
        reap_reserve((usize)((double)num_pages * 0.2)),
        shard_reap_reserve(reap_reserve / num_shards) {}

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

struct alignas(64) Shard {
  std::mutex mutex;

  // Lists used to organize the pages mapped in the cache with a LRU policy
  std::list<Page *> active_pages;
  std::list<Page *> inactive_pages;
};

struct ShardStats {
  std::atomic<usize> faults = 0;
  std::atomic<usize> evictions = 0;
  std::atomic<usize> lock_contentions = 0;
  std::atomic<usize> promotions = 0;
  std::atomic<usize> demotions = 0;

  // Sampled by rebalancer
  usize active_size_sum = 0;
  usize inactive_size_sum = 0;
  usize samples = 0;
};

struct RebalancerStats {
  usize total_cycles = 0;
  usize pressure_events = 0;
  usize relaxation_events = 0;
  usize current_interval_ms = 0;
  usize current_stable_cycles = 0;
  usize time_at_min_ms = 0;
  usize time_at_max_ms = 0;
};

struct SwapperStats {
  std::atomic<usize> total_faults = 0;
  std::atomic<usize> demand_zeros = 0;
  std::atomic<usize> swap_ins = 0;
  // evictions of dirty pages
  std::atomic<usize> swap_outs = 0;
  // evictions of Mapped clean pages
  std::atomic<usize> skipped_writes = 0;
  // evictions of FreshlyMapped clean pages
  std::atomic<usize> discards = 0;
  // evictions that happened because no free slot was find in the cache
  std::atomic<usize> reactive_evictions = 0;
  // evictions that happened because of the background reaper
  std::atomic<usize> proactive_evictions = 0;
  std::atomic<usize> promotions = 0;
  std::atomic<usize> demotions = 0;
  std::atomic<usize> rebalancer_skips = 0;

  std::unique_ptr<ShardStats[]> shard_stats;
  RebalancerStats rebalancer_stats;
};

class Swapper {
public:
  Swapper(SwapperConfig &&swapper_config, std::unique_ptr<Storage> storage);
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
  // Returns the index of the shard where a page is handled
  usize get_shard_idx(uptr vaddr);

  // Acquires a free physical page slot to service a page fault.
  Page *acquire_page(Shard &shard);

  std::unique_ptr<Storage> storage_; // The abstract storage backend
  void *cache_base_addr_;            // The base address of the cache
  // Fixed-size array describing where a page is located in the cache
  std::unique_ptr<Page[]> pages_;
  // Unmapped slots in the cache
  moodycamel::ConcurrentQueue<Page *> free_pages_queue_;
  // Split the cache into smaller shards each with their own two-list LRU
  std::unique_ptr<Shard[]> shards_;

  // Track the state of all pages, both on the cache and on the swap area
  std::unique_ptr<std::atomic<PageState>[]> page_states_;

  std::thread rebalancer_;
  std::atomic<bool> rebalancer_running_{true};
  // how often the rebalance thread runs, recomputed at runtime
  u32 rebalance_interval_ms_ = (MAX_INTERVAL_MS + MIN_INTERVAL_MS) / 2;
  // shared between fault handler and the rebalance thread
  std::atomic<u32> evictions_in_cycle_ = 0;
  // how many cycles passed without the pressure of too many evictions
  u32 stable_cycles_ = 0;
  void adapt_rebalance_interval();
  // AIMD constants
  static constexpr u32 MIN_INTERVAL_MS = 10;   // most aggressive
  static constexpr u32 MAX_INTERVAL_MS = 2000; // most relaxed
  static constexpr u32 ADDITIVE_INCREASE_MS = 10;
  static constexpr float MULTIPLICATIVE_DECREASE_FACTOR = 0.5;
  // react if >n sync evictions happen in one cycle
  static constexpr u8 PRESSURE_THRESHOLD = 3;
  // relax only after n good cycles in a row
  static constexpr u8 COOLDOWN_CYCLES = 6;

  SwapperStats stats_;
};
