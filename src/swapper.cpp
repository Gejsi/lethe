#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <random>
#include <volimem/mapper.h>

#define USE_ASYNC_LOGGER
#include "swapper.h"
#include "utils.h"

Arena *ArenaQueueTraits::arena = nullptr;

constexpr bool pte_is_present(u64 pte) { return pte & PTE_P; }
constexpr bool pte_is_writable(u64 pte) { return pte & PTE_W; }
constexpr bool pte_is_accessed(u64 pte) { return pte & PTE_A; }
constexpr bool pte_is_dirty(u64 pte) { return pte & PTE_D; }

void set_permissions(uptr vaddr, u64 flags) {
  mapper_t::mpermit(vaddr, PAGE_SIZE, flags);
  mapper_t::flush(vaddr, PAGE_SIZE);
}

void clear_permissions(uptr vaddr, u64 flags) {
  auto perms = mapper_t::get_protect(vaddr);
  mapper_t::mpermit(vaddr, PAGE_SIZE, perms & ~flags);
  mapper_t::flush(vaddr, PAGE_SIZE);
}

// Map a virtual page to a physical page
void map_gva(uptr gva, uptr gpa) {
  // mapper_t::map_gpt(gva, gpa, PAGE_SIZE, PTE_P | PTE_W);
  mapper_t::double_map(gva, gva, PAGE_SIZE);
}

// Unmap a page from the guest page table
void unmap_gva(uptr gva) {
  // unmap the page from the guest page table
  mapper_t::unmap(gva, PAGE_SIZE);
  // ensure that unmap actually invalidates the TLB entry
  mapper_t::flush(gva, PAGE_SIZE);
}

constexpr usize vaddr_to_vpn(uptr vaddr) { return vaddr / PAGE_SIZE; }
constexpr uptr vpn_to_vaddr(usize vpn) { return vpn * PAGE_SIZE; }

PageState Shard::get_page_state(uptr vaddr) {
  usize vpn = vaddr_to_vpn(vaddr);

  auto it = page_states.find(vpn);
  if (it == page_states.end()) {
    return PageState::Unmapped;
  }

  return it->second;
}

void Shard::set_page_state(uptr vaddr, PageState page_state) {
  usize vpn = vaddr_to_vpn(vaddr);
  page_states[vpn] = page_state;
}

Swapper::Swapper(SwapperConfig &&swapper_config,
                 std::unique_ptr<Storage> storage)
    : config(std::move(swapper_config)), storage_(std::move(storage)),
      pages_(std::make_unique<Page[]>(config.num_pages)),
      metadata_arena_(512 * MB) {
  // AsyncLogger::instance().init("swapper.log");

  cache_base_addr_ = storage_->get_cache_base_addr();
  if (!cache_base_addr_) {
    PANIC(
        "Swapper initialized with a storage backend that has no cache address");
  }

  ArenaQueueTraits::arena = &metadata_arena_;
  free_pages_queue_ =
      std::make_unique<moodycamel::ConcurrentQueue<Page *, ArenaQueueTraits>>();

  for (usize i = 0; i < config.num_pages; i++) {
    free_pages_queue_->enqueue(&pages_[i]);
    // free_pages_queue_.push(&pages_[i]);
  }

  // manually allocate memory to hold the shards
  shards_ = static_cast<Shard *>(operator new[](
      sizeof(Shard) * config.num_shards, std::align_val_t{alignof(Shard)}));

  // placement-new each shard
  for (usize i = 0; i < config.num_shards; ++i) {
    new (&shards_[i]) Shard(&metadata_arena_);
  }

  // for (usize i = 0; i < config.num_heap_pages; ++i) {
  //   page_states_[i].store(PageState::Unmapped, std::memory_order_relaxed);
  // }

  mem_fd_ = open("/proc/self/mem", O_RDONLY);
  if (mem_fd_ < 0)
    PANIC("Could not open /proc/self/mem");

  INFO("Swapper initialized with %zu cache slots and %zu shards to handle them",
       config.num_pages, config.num_shards);
}

Swapper::~Swapper() {
  // manually deallocate shards
  for (usize i = 0; i < config.num_shards; ++i) {
    shards_[i].~Shard();
  }
  operator delete[](shards_, std::align_val_t{alignof(Shard)});

  DEBUG("Destroyed Swapper");
}

usize Swapper::get_page_idx(Page *page) { return (usize)(page - pages_.get()); }

uptr Swapper::get_cache_gva(Page *page) {
  usize page_idx = get_page_idx(page);
  auto cache_offset = page_idx * PAGE_SIZE;
  return (uptr)cache_base_addr_ + cache_offset;
}

usize Swapper::get_shard_idx(uptr vaddr) {
  return vaddr_to_vpn(vaddr) % config.num_shards;
}

Page *Swapper::acquire_page(Shard &shard) {
  Page *victim_page = nullptr;

  if (free_pages_queue_->try_dequeue(victim_page)) {
    return victim_page;
  }
  // if (!free_pages_queue_.empty()) {
  //   victim_page = free_pages_queue_.front();
  //   free_pages_queue_.pop();
  //   return victim_page;
  // }

  // no free page found, eviction is necessary
  if (!shard.inactive_pages.empty()) {
    victim_page = shard.inactive_pages.back();
    shard.inactive_pages.pop_back();
    DEBUG("Free list empty. Chose victim GVA 0x%lx from inactive list",
          victim_page->vaddr);
  } else if (!shard.active_pages.empty()) {
    victim_page = shard.active_pages.back();
    shard.active_pages.pop_back();
    DEBUG("Free list empty. Chose victim GVA 0x%lx from active list",
          victim_page->vaddr);
  } else {
    UNREACHABLE("No pages available to evict from any list");
  }

  // evict chosen page
  swap_out_page(victim_page);

  // increase count of evictions to possibly
  // adapt the frequency of the rebalancing
  evictions_in_cycle_++;

  stats_.reactive_evictions++;

  return victim_page;
}

void Swapper::swap_in_page(Page *page, uptr fault_vaddr) {
  usize shard_idx = get_shard_idx(fault_vaddr);
  Shard &shard = shards_[shard_idx];
  // auto &page_state =
  //     shard.page_states.try_emplace(fault_vaddr, PageState::Unmapped)
  //         .first->second;
  PageState page_state = shard.get_page_state(fault_vaddr);

  auto cache_gva = get_cache_gva(page);
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);

  if (page_state == PageState::Unmapped) {
    DEBUG("ASSIGNING page for gva 0x%lx", fault_vaddr);

    // The Kernel might have written data here (via read/recv syscalls).
    // pread from /proc/self/mem asks the kernel: "Give me the data at
    // fault_vaddr". If the kernel allocated a page there, we get the data.
    ssize_t bytes =
        pread(mem_fd_, (void *)cache_gva, PAGE_SIZE, (off_t)fault_vaddr);

    if (bytes != PAGE_SIZE) {
      // Kernel has nothing there. Standard Demand Zero.
      memset((void *)cache_gva, 0, PAGE_SIZE);
    } else {
      // Kernel had data! We just copied it into our Cache Page.
      // We effectively "adopted" the data into our swapper system.
    }

    shard.set_page_state(fault_vaddr, PageState::FreshlyMapped);
    stats_.demand_zeros++;
  } else if (page_state == PageState::RemotelyMapped) {
    DEBUG("Swapping IN: 0x%lx into cache slot %zu. Cache (gva->gpa): "
          "0x%lx->0x%lx",
          fault_vaddr, get_page_idx(page), cache_gva, cache_gpa);

    // auto target_offset = fault_vaddr - HEAP_START;
    auto target_offset = shard.remote_offsets[vaddr_to_vpn(fault_vaddr)];
    int ret = storage_->read_page((void *)cache_gva, target_offset);
    if (ret != 0) {
      PANIC("Failed to swap in: backend read failed (%d)", ret);
    }

    shard.set_page_state(fault_vaddr, PageState::Mapped);

    stats_.swap_ins++;
  } else {
    UNREACHABLE(
        "A %s page (0x%lx) shouldn't cause a fault that needs a swap-in",
        page_state_to_str(page_state), fault_vaddr);
  }

  map_gva(fault_vaddr, cache_gpa);
  page->vaddr = fault_vaddr;
}

void Swapper::swap_out_page(Page *page) {
  uptr victim_vaddr = page->vaddr;
  usize shard_idx = get_shard_idx(victim_vaddr);
  Shard &shard = shards_[shard_idx];
  PageState page_state = shard.get_page_state(victim_vaddr);

  auto perms = mapper_t::get_protect(victim_vaddr);
  bool is_dirty = pte_is_dirty(perms);

  unmap_gva(victim_vaddr);

  if (is_dirty) {
    // dirty page: write to storage
    // auto victim_offset = victim_vaddr - HEAP_START;
    u64 victim_offset;
    usize vpn = vaddr_to_vpn(victim_vaddr);
    auto offset_it = shard.remote_offsets.find(vpn);
    if (offset_it == shard.remote_offsets.end()) {
      victim_offset = next_swap_offset_.fetch_add(PAGE_SIZE);
      shard.remote_offsets[vpn] = victim_offset;
    } else {
      // Reuse existing offset
      victim_offset = offset_it->second;
    }

    auto cache_gva = get_cache_gva(page);
    DEBUG("Swapping OUT: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx", victim_vaddr,
          cache_gva, mapper_t::gva_to_gpa((void *)cache_gva));

    int ret = storage_->write_page((void *)cache_gva, victim_offset);
    if (ret != 0) {
      PANIC("Failed to swap out: backend write failed (%d)", ret);
    }

    shard.set_page_state(victim_vaddr, PageState::RemotelyMapped);

    stats_.swap_outs++;
  } else if (page_state == PageState::Mapped) {
    // clean page whose data is backed on the storage
    DEBUG("Evicting clean storage-backed page 0x%lx from cache slot %zu",
          victim_vaddr, get_page_idx(page));

    shard.set_page_state(victim_vaddr, PageState::RemotelyMapped);

    stats_.skipped_writes++;
  } else if (page_state == PageState::FreshlyMapped) {
    // clean page that was never written: it can be discarded
    DEBUG("Evicting fresh page 0x%lx from cache slot %zu", victim_vaddr,
          get_page_idx(page));

    shard.set_page_state(victim_vaddr, PageState::Unmapped);

    stats_.discards++;
  } else {
    UNREACHABLE(
        "A %s page (0x%lx) shouldn't cause a fault that needs a swap-out",
        page_state_to_str(page_state), victim_vaddr);
  }

  page->reset();
}

// void Swapper::handle_alloc(u64 vaddr, u64 len) {
//   for (uptr v = vaddr; v < vaddr + len; v += PAGE_SIZE) {
//     usize shard_idx = get_shard_idx(v);
// Shard &shard = shards_[shard_idx];
// std::lock_guard<std::mutex> lock(shard.mutex);
// shard.page_states[v] = PageState::Unmapped;
//   }
// }

void Swapper::handle_fault(void *fault_addr, regstate_t *regstate) {
  UNUSED(regstate);

  DEBUG("Handling fault at %p. Error code: %lu", fault_addr,
        regstate->error_code);

  stats_.total_faults++;

  uptr aligned_fault_vaddr = ALIGN_DOWN((uptr)fault_addr);
  usize shard_idx = get_shard_idx(aligned_fault_vaddr);
  Shard &shard = shards_[shard_idx];

  std::lock_guard<std::mutex> lock(shard.mutex);
  Page *page = acquire_page(shard);
  swap_in_page(page, aligned_fault_vaddr);
  shard.active_pages.push_front(page);
}

void Swapper::start_background_rebalancing() {
  if (config.rebalancer_disabled) {
    return;
  }

  rebalancer_ = std::thread([this]() {
    std::vector<usize> shard_indices(config.num_shards);
    for (usize i = 0; i < config.num_shards; ++i) {
      shard_indices[i] = i;
    }

    std::random_device rd;
    std::default_random_engine rng(rd());

    while (rebalancer_running_.load()) {
      sleep_ms(rebalance_interval_ms_);

      // Shuffle the order of shards to rebalance
      std::shuffle(shard_indices.begin(), shard_indices.end(), rng);

      for (usize shard_idx : shard_indices) {
        Shard &shard = shards_[shard_idx];

        std::unique_lock<std::mutex> lock(shard.mutex, std::try_to_lock);

        if (lock.owns_lock()) {
          // Rebalance lists
          demote_cold_pages(shard);
          promote_hot_pages(shard);
          reap_cold_pages(shard);
        } else {
          stats_.rebalancer_skips++;
        }

        if (!rebalancer_running_.load()) {
          return;
        }
      }

      // Adapt the interval at which this thread runs
      adapt_rebalance_interval();
    }
  });
}

void Swapper::stop_background_rebalancing() {
  if (config.rebalancer_disabled) {
    return;
  }

  rebalancer_running_ = false;

  if (rebalancer_.joinable()) {
    rebalancer_.join();
  }
}

void Swapper::demote_cold_pages(Shard &shard) {
  // iterate from the back (warm pages) to the front (hottest pages)
  for (auto it = shard.active_pages.rbegin();
       it != shard.active_pages.rend();) {
    auto hot_page = *it;

    if (auto perms = mapper_t::get_protect(hot_page->vaddr);
        pte_is_accessed(perms)) {
      // page is still hot, clear the A-bit and keep it in the active list
      clear_permissions(hot_page->vaddr, PTE_A);
      // move page to front
      shard.active_pages.splice(shard.active_pages.begin(), shard.active_pages,
                                std::next(it).base());

      ++it;
    } else {
      // page has become cold, demote it to the inactive list
      shard.inactive_pages.push_front(hot_page);

      // basically like calling `erase` but in reverse
      it = std::list<Page *>::reverse_iterator(
          shard.active_pages.erase(std::next(it).base()));

      stats_.demotions++;
    }
  }
}

void Swapper::promote_hot_pages(Shard &shard) {
  for (auto it = shard.inactive_pages.begin();
       it != shard.inactive_pages.end();) {
    auto cold_page = *it;

    if (auto perms = mapper_t::get_protect(cold_page->vaddr);
        pte_is_accessed(perms)) {
      // page has become hot, promote to active
      shard.active_pages.push_front(cold_page);
      it = shard.inactive_pages.erase(it);
      clear_permissions(cold_page->vaddr, PTE_A);

      stats_.promotions++;
    } else {
      // page is still cold, keep it in the inactive list
      ++it;
    }
  }
}

void Swapper::reap_cold_pages(Shard &shard) {
  if (free_pages_queue_->size_approx() >= config.reap_reserve) {
    return;
  }

  // if (free_pages_queue_.size() >= config.reap_reserve) {
  //   return;
  // }

  usize evictions = 0;

  // try to keep some amount of free space in the shard
  while (!shard.inactive_pages.empty() &&
         evictions < config.shard_reap_reserve) {
    Page *victim_page = shard.inactive_pages.back();
    shard.inactive_pages.pop_back();

    swap_out_page(victim_page);
    free_pages_queue_->enqueue(victim_page);
    // free_pages_queue_.push(victim_page);

    evictions++;
    stats_.proactive_evictions++;
  }
}

void Swapper::adapt_rebalance_interval() {
  // atomically get and reset the counter
  if (usize evictions = evictions_in_cycle_.exchange(0);
      evictions > PRESSURE_THRESHOLD) {
    // sustained pressure: multiplicative decrease
    u32 new_interval =
        (u32)((float)rebalance_interval_ms_ * MULTIPLICATIVE_DECREASE_FACTOR);
    rebalance_interval_ms_ = std::max(MIN_INTERVAL_MS, new_interval);

    INFO("[ADAPT] High pressure due to %zu evictions: decreased rebalancing "
         "frequency to %u ms",
         evictions, rebalance_interval_ms_);

    // reset the cooldown counter
    stable_cycles_ = 0;
  } else {
    stable_cycles_++;

    // stable system: additive increase
    if (stable_cycles_ >= COOLDOWN_CYCLES) {
      auto cur = rebalance_interval_ms_;
      u32 new_interval = cur + ADDITIVE_INCREASE_MS;
      rebalance_interval_ms_ = std::min(MAX_INTERVAL_MS, new_interval);

      INFO("[ADAPT] Stable system: increased rebalancing frequency to %u ms",
           rebalance_interval_ms_);

      // reset the counter to require another
      // full cooldown period before the next increase
      stable_cycles_ = 0;
    }

    // if the cooldown threshold wasn't reached,
    // maintain the current interval
  }
}

void Swapper::print_stats() {
  usize total_faults = stats_.total_faults.load();
  usize swap_ins = stats_.swap_ins.load();
  usize swap_outs = stats_.swap_outs.load();
  usize skipped_writes = stats_.skipped_writes.load();
  usize discards = stats_.discards.load();
  usize reactive_evictions = stats_.reactive_evictions.load();
  usize proactive_evictions = stats_.proactive_evictions.load();
  usize promotions = stats_.promotions.load();

  // total evictions, categorized by content (dirty/clean)
  usize total_evictions_by_content = swap_outs + skipped_writes + discards;
  // total evictions, categorized by trigger (proactive/reactive)
  usize total_evictions_by_trigger = proactive_evictions + reactive_evictions;
  ENSURE(total_evictions_by_content == total_evictions_by_trigger,
         "Number of total evictions doesn't match");

  double swap_in_ratio =
      (total_faults > 0)
          ? static_cast<double>(swap_ins) / static_cast<double>(total_faults)
          : 0.0;

  double churn_rate = (total_evictions_by_content > 0)
                          ? static_cast<double>(swap_ins) /
                                static_cast<double>(total_evictions_by_content)
                          : 0.0;

  double dirty_ratio = (total_evictions_by_content > 0)
                           ? static_cast<double>(swap_outs) /
                                 static_cast<double>(total_evictions_by_content)
                           : 0.0;

  double proactive_efficiency =
      (total_evictions_by_trigger > 0)
          ? static_cast<double>(proactive_evictions) /
                static_cast<double>(total_evictions_by_trigger)
          : 0.0;

  double stall_rate = (total_evictions_by_trigger > 0)
                          ? static_cast<double>(reactive_evictions) /
                                static_cast<double>(total_evictions_by_trigger)
                          : 0.0;

  double promotion_ratio =
      (promotions + total_evictions_by_content > 0)
          ? static_cast<double>(promotions) /
                static_cast<double>(promotions + total_evictions_by_content)
          : 0.0;

  printf("=================================================================\n");
  printf("PERFORMANCE METRICS:\n");
  printf("  - Swap-In Ratio:        %.2f%% (%.0f / %.0f faults were for "
         "remote pages)\n",
         swap_in_ratio * 100.0, static_cast<double>(swap_ins),
         static_cast<double>(total_faults));
  printf("  - Swap-Out Ratio:       %.2f%% (%.0f / %.0f evictions required a "
         "write-back)\n",
         dirty_ratio * 100.0, static_cast<double>(swap_outs),
         static_cast<double>(total_evictions_by_content));
  printf("  - Proactive Efficiency: %.2f%% (%.0f / %.0f total evictions were "
         "proactive)\n",
         proactive_efficiency * 100.0, static_cast<double>(proactive_evictions),
         static_cast<double>(total_evictions_by_trigger));
  printf("  - Stall Rate:           %.2f%% (%.0f / %.0f total evictions "
         "stalled the app)\n",
         stall_rate * 100.0, static_cast<double>(reactive_evictions),
         static_cast<double>(total_evictions_by_trigger));
  printf("  - Churn Rate:           %.2f%% (%.0f / %.0f swap-ins over "
         "evictions)\n",
         churn_rate * 100.0, static_cast<double>(swap_ins),
         static_cast<double>(total_evictions_by_content));
  printf("  - Promotion Ratio:      %.2f%% (%.0f / %.0f inactive pages were "
         "promoted)\n",
         promotion_ratio * 100.0, static_cast<double>(promotions),
         static_cast<double>(promotions + total_evictions_by_content));

  printf("\nCOUNTERS:\n");
  printf("  - Total Page Faults:    %zu\n", total_faults);
  printf("    - Demand Zeros:       %zu\n", stats_.demand_zeros.load());
  printf("    - Swap-Ins:           %zu\n", swap_ins);
  printf("  - Total Evictions:      %zu\n", total_evictions_by_content);
  printf("    - Swap-Outs:          %zu\n", swap_outs);
  printf("    - Skipped writes:     %zu\n", skipped_writes);
  printf("    - Discards:           %zu\n", discards);
  printf("  - Eviction Triggers:\n");
  printf("    - Proactive (Reaper): %zu\n", proactive_evictions);
  printf("    - Reactive (Stall):   %zu\n", reactive_evictions);
  printf("  - LRU List Activity:\n");
  printf("    - Promotions:         %zu\n", promotions);
  printf("    - Demotions:          %zu\n", stats_.demotions.load());
  printf("    - Rebalancer skips:   %zu\n", stats_.rebalancer_skips.load());
  printf("=================================================================\n");
}
