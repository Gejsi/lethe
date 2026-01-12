#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <volimem/idt.h>

#include "arena.h"
#include "concurrentqueue.h"
#include "storage/storage.h"
#include "utils.h"

constexpr usize SWAP_SIZE = 512 * MB;
// constexpr usize SWAP_SIZE = 1 * GB;
constexpr uptr HEAP_START = 0xffff800000000000;
// constexpr uptr HEAP_START = 0x0000555500000000UL;

struct SwapperConfig {
  usize cache_size = 128 * MB;
  usize num_shards = 16;
  bool rebalancer_disabled = false;

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
  // NOTE: keep this entry as the first one in the enum
  // Cache slot is empty: access triggers a demand-zero allocation.
  Unmapped,

  // Page is in cache but has NO copy on the storage:
  // basically, a newly allocated demand-zero page.
  // If evicted while clean, it can be fully discarded
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

  void reset() { *this = Page{}; }

  void print(bool inline_output = true) const {
    if (inline_output) {
      printf("Page { vaddr: 0x%lx }\n", vaddr);
    } else {
      printf("Page {\n");
      printf("  vaddr: 0x%lx\n", vaddr);
      printf("}\n");
    }
  }
};

struct alignas(64) Shard {
  std::mutex mutex;

  // Lists used to organize the pages mapped in the cache with a LRU policy
  ArenaList<Page *> active_pages;
  ArenaList<Page *> inactive_pages;

  // Track the state of pages, both on the cache and on the swap area
  ArenaUnorderedMap<usize, PageState> page_states;

  ArenaUnorderedMap<usize, u64> remote_offsets;

  // save the oldest page (back of inactive list).
  // 0 = Empty, UINT64_MAX = Newest.
  // TODO: use this field
  // std::atomic<u64> oldest_age{0};

  Shard(Arena *arena)
      : active_pages(ArenaAllocator<Page *>(arena)),
        inactive_pages(ArenaAllocator<Page *>(arena)),
        page_states(ArenaAllocator<std::pair<const usize, PageState>>(arena)),
        remote_offsets(ArenaAllocator<std::pair<const usize, u64>>(arena)) {}

  PageState get_page_state(uptr vaddr);
  void set_page_state(uptr vaddr, PageState page_state);
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

struct ArenaQueueTraits : moodycamel::ConcurrentQueueDefaultTraits {
  static Arena *arena;

  static inline void *malloc(usize size) {
    if (!arena) {
      PANIC("Concurrent queue must use an arena allocator");
    }

    constexpr usize alignment = alignof(std::max_align_t);
    uptr ptr = arena->allocate(size, alignment);
    if (ptr == 0) {
      return nullptr;
    }

    return reinterpret_cast<void *>(ptr);
  }

  static inline void free(void *) {
    // no-op
  }
};

class Swapper {
public:
  Swapper(SwapperConfig &&swapper_config, std::unique_ptr<Storage> storage);
  ~Swapper();

  // The main entry point called by VoliMem on a page fault
  void handle_fault(void *fault_addr, regstate_t *regstate);

  void handle_alloc(u64 vaddr, u64 len);
  void handle_dealloc(u64 vaddr, u64 len);

  // Starts the LRU-rebalancing background thread
  void start_background_rebalancing();
  // Stops the LRU-rebalancing background thread
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
  // TODO: improve error handling of these two methods
  void swap_in_page(Page *page, uptr fault_vaddr);
  void swap_out_page(Page *page);

  // Returns the index of the cache slot where a page is located
  usize get_page_idx(Page *page);
  // Returns the GVA associated with a page
  uptr get_cache_gva(Page *page);
  // Returns the index of the shard where a page is handled
  usize get_shard_idx(uptr vaddr);

  // Acquires a free physical page slot to service a page fault.
  Page *acquire_page(Shard &shard);

  std::unique_ptr<Storage> storage_; // The abstract storage backend
  void *cache_base_addr_;            // The base address of the cache
  // Fixed-size array describing where a faulting address
  // is located in the physical cache
  std::unique_ptr<Page[]> pages_;

  // 512MB for metadata allows tracking ~32GB of RAM,
  // assuming 16 bytes overhead per 4KB page
  Arena metadata_arena_;

  std::unique_ptr<moodycamel::ConcurrentQueue<Page *, ArenaQueueTraits>>
      free_pages_queue_;

  // std::queue<Page *, std::list<Page *, ArenaAllocator<Page *>>>
  //     free_pages_queue_;

  // std::unique_ptr<Shard[]> shards_;
  // Contiguous array of shards whose allocation must be manually managed
  Shard *shards_;

  // TODO: Use a more appropriate way of handling remote offsets.
  // Pages that have been swapped out need a remote offset assigned:
  // the easiest approach is simply bumping an offset
  // at the cost of leaking memory.
  std::atomic<u64> next_swap_offset_{0};

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
