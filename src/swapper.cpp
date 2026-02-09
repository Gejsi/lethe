#include <algorithm>
#include <cstdio>
#include <mutex>
#include <random>
#include <volimem/mapper.h>

#define USE_ASYNC_LOGGER
#include "swapper.h"

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

void map_gva(uptr gva, uptr gpa) {
  mapper_t::map_gpt(gva, gpa, PAGE_SIZE, PTE_P | PTE_W);
}

void unmap_gva(uptr gva) {
  // unmap the page from the guest page table
  mapper_t::unmap(gva, PAGE_SIZE);
  // ensure that unmap actually invalidates the TLB entry
  mapper_t::flush(gva, PAGE_SIZE);
}

Swapper::Swapper(SwapperConfig &&swapper_config,
                 std::unique_ptr<Storage> storage)
    : config(std::move(swapper_config)), storage_(std::move(storage)),
      cache_base_addr_(storage_->get_cache_base_addr()),
      pages_(std::make_unique<Page[]>(config.num_pages)),
      shards_(std::make_unique<Shard[]>(config.num_shards)),
      page_states_(
          std::make_unique<std::atomic<PageState>[]>(config.num_heap_pages)) {
  for (usize i = 0; i < config.num_pages; i++) {
    free_pages_queue_.enqueue(&pages_[i]);
  }

  stats_.shard_stats = std::make_unique<ShardStats[]>(config.num_shards);

  INFO("Swapper initialized with %zu cache slots and %zu shards to handle them",
       config.num_pages, config.num_shards);
}

Swapper::~Swapper() { DEBUG("Destroyed Swapper"); }

usize Swapper::get_page_idx(Page *page) { return (usize)(page - pages_.get()); }

uptr Swapper::get_cache_gva(Page *page) {
  usize page_idx = get_page_idx(page);
  auto cache_offset = page_idx * PAGE_SIZE;
  return (uptr)cache_base_addr_ + cache_offset;
}

usize Swapper::get_heap_idx(uptr vaddr) {
  return (vaddr - HEAP_START) / PAGE_SIZE;
}

usize Swapper::get_shard_idx(uptr vaddr) {
  return (vaddr / PAGE_SIZE) % config.num_shards;
}

Page *Swapper::acquire_page(Shard &shard) {
  Page *victim_page = nullptr;

  if (free_pages_queue_.try_dequeue(victim_page)) {
    return victim_page;
  }

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

void Swapper::swap_in_page(Page *page, uptr aligned_fault_vaddr) {
  usize heap_idx = get_heap_idx(aligned_fault_vaddr);
  PageState page_state = page_states_[heap_idx].load();

  auto cache_gva = get_cache_gva(page);
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);

  if (page_state == PageState::Unmapped) {
    DEBUG("Assigning page for gva 0x%lx", aligned_fault_vaddr);
    memset((void *)cache_gva, 0, PAGE_SIZE);
    page_states_[heap_idx] = PageState::FreshlyMapped;

    stats_.demand_zeros++;
  } else if (page_state == PageState::RemotelyMapped) {
    DEBUG("Swapping IN: 0x%lx into cache slot %zu. Cache (gva->gpa): "
          "0x%lx->0x%lx",
          aligned_fault_vaddr, get_page_idx(page), cache_gva, cache_gpa);

    auto target_offset = aligned_fault_vaddr - HEAP_START;
    int ret = storage_->read_page((void *)cache_gva, target_offset);
    if (ret != 0) {
      PANIC("Failed to swap in: backend read failed (%d)", ret);
    }

    page_states_[heap_idx] = PageState::Mapped;

    stats_.swap_ins++;
  } else {
    UNREACHABLE(
        "A %s page (0x%lx) shouldn't cause a fault that needs a swap-in",
        page_state_to_str(page_state), aligned_fault_vaddr);
  }

  map_gva(aligned_fault_vaddr, cache_gpa);
  page->vaddr = aligned_fault_vaddr;
}

void Swapper::swap_out_page(Page *page) {
  // victim page is the n-th page within the heap
  uptr victim_vaddr = page->vaddr;
  usize heap_idx = get_heap_idx(victim_vaddr);
  PageState page_state = page_states_[heap_idx].load();

  auto perms = mapper_t::get_protect(victim_vaddr);
  bool is_dirty = pte_is_dirty(perms);

  unmap_gva(victim_vaddr);

  if (is_dirty) {
    // dirty page: write to storage
    auto victim_offset = victim_vaddr - HEAP_START;
    auto cache_gva = get_cache_gva(page);
    DEBUG("Swapping OUT: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx", victim_vaddr,
          cache_gva, mapper_t::gva_to_gpa((void *)cache_gva));

    int ret = storage_->write_page((void *)cache_gva, victim_offset);
    if (ret != 0) {
      PANIC("Failed to swap out: backend write failed (%d)", ret);
    }

    page_states_[heap_idx] = PageState::RemotelyMapped;

    stats_.swap_outs++;
  } else if (page_state == PageState::Mapped) {
    // clean page whose data is backed on the storage
    DEBUG("Evicting clean storage-backed page 0x%lx from cache slot %zu",
          victim_vaddr, get_page_idx(page));

    page_states_[heap_idx] = PageState::RemotelyMapped;

    stats_.skipped_writes++;
  } else if (page_state == PageState::FreshlyMapped) {
    // clean page that was never written: it can be discarded
    DEBUG("Evicting fresh page 0x%lx from cache slot %zu", victim_vaddr,
          get_page_idx(page));

    page_states_[heap_idx] = PageState::Unmapped;

    stats_.discards++;
  } else {
    UNREACHABLE(
        "A %s page (0x%lx) shouldn't cause a fault that needs a swap-out",
        page_state_to_str(page_state), victim_vaddr);
  }

  page->reset();
}

void Swapper::handle_fault(void *fault_addr, regstate_t *regstate) {
  UNUSED(regstate);

  DEBUG("Handling fault at %p. Error code: %lu", fault_addr,
        regstate->error_code);

  // align to page boundary
  uptr aligned_fault_vaddr = ALIGN_DOWN((uptr)fault_addr);

  // faulting address should be within the heap
  if (aligned_fault_vaddr < HEAP_START ||
      aligned_fault_vaddr >= HEAP_START + config.heap_size) {
    PANIC("Fault address 0x%lx outside of heap range [0x%lx, 0x%lx)",
          aligned_fault_vaddr, HEAP_START, HEAP_START + config.heap_size);
  }

  stats_.total_faults++;

  usize shard_idx = get_shard_idx(aligned_fault_vaddr);
  Shard &shard = shards_[shard_idx];
  stats_.shard_stats[shard_idx].faults++;

  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    Page *page = acquire_page(shard);
    swap_in_page(page, aligned_fault_vaddr);
    shard.active_pages.push_front(page);
  }
}

void Swapper::start_background_rebalancing() {
  if (config.rebalancer_disabled) {
    return;
  }

  rebalancer_ = std::thread([this]() {
    std::vector<usize> shard_indices(config.num_shards);
    for (usize i = 0; i < config.num_shards; ++i)
      shard_indices[i] = i;

    std::random_device rd;
    std::default_random_engine rng(rd());

    while (rebalancer_running_.load()) {
      auto cycle_start = Clock::now();

      sleep_ms(rebalance_interval_ms_);

      // Shuffle the order of shards to rebalance
      std::shuffle(shard_indices.begin(), shard_indices.end(), rng);

      for (usize shard_idx : shard_indices) {
        Shard &shard = shards_[shard_idx];

        std::unique_lock<std::mutex> lock(shard.mutex, std::try_to_lock);

        if (lock.owns_lock()) {
          stats_.shard_stats[shard_idx].active_size_sum +=
              shard.active_pages.size();
          stats_.shard_stats[shard_idx].inactive_size_sum +=
              shard.inactive_pages.size();
          stats_.shard_stats[shard_idx].samples++;

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

      stats_.rebalancer_stats.total_cycles++;
      stats_.rebalancer_stats.current_interval_ms = rebalance_interval_ms_;

      // Track time spent at extremes
      if (rebalance_interval_ms_ == MIN_INTERVAL_MS) {
        auto cycle_duration =
            std::chrono::duration_cast<Milliseconds>(Clock::now() - cycle_start)
                .count();
        stats_.rebalancer_stats.time_at_min_ms += (usize)cycle_duration;
      } else if (rebalance_interval_ms_ == MAX_INTERVAL_MS) {
        auto cycle_duration =
            std::chrono::duration_cast<Milliseconds>(Clock::now() - cycle_start)
                .count();
        stats_.rebalancer_stats.time_at_max_ms += (usize)cycle_duration;
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
  usize shard_idx = static_cast<usize>(&shard - &shards_[0]);
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
      stats_.shard_stats[shard_idx].demotions++;
    }
  }
}

void Swapper::promote_hot_pages(Shard &shard) {
  usize shard_idx = static_cast<usize>(&shard - &shards_[0]);

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
      stats_.shard_stats[shard_idx].promotions++;
    } else {
      // page is still cold, keep it in the inactive list
      ++it;
    }
  }
}

void Swapper::reap_cold_pages(Shard &shard) {
  if (free_pages_queue_.size_approx() >= config.reap_reserve) {
    return;
  }

  usize evictions = 0;

  // try to keep some amount of free space in the shard
  while (!shard.inactive_pages.empty() &&
         evictions < config.shard_reap_reserve) {
    Page *victim_page = shard.inactive_pages.back();
    shard.inactive_pages.pop_back();

    swap_out_page(victim_page);
    free_pages_queue_.enqueue(victim_page);

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

    stats_.rebalancer_stats.pressure_events++;

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

      stats_.rebalancer_stats.relaxation_events++;

      // reset the counter to require another
      // full cooldown period before the next increase
      stable_cycles_ = 0;
    }

    // if the cooldown threshold wasn't reached,
    // maintain the current interval
  }

  stats_.rebalancer_stats.current_stable_cycles = stable_cycles_;
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

  usize total_evictions = swap_outs + skipped_writes + discards;

  // Calculate metrics
  double swap_in_ratio =
      (total_faults > 0) ? (double)swap_ins / (double)total_faults : 0.0;
  double working_set_hit_rate = 1.0 - swap_in_ratio;
  double dirty_ratio =
      (total_evictions > 0) ? (double)swap_outs / (double)total_evictions : 0.0;
  double pollution_rate =
      (total_evictions > 0) ? (double)discards / (double)total_evictions : 0.0;
  double proactive_efficiency =
      (total_evictions > 0)
          ? (double)proactive_evictions / (double)total_evictions
          : 0.0;
  double stall_rate = (total_evictions > 0)
                          ? (double)reactive_evictions / (double)total_evictions
                          : 0.0;

  printf("=================================================================\n");
  printf("PERFORMANCE METRICS:\n");
  printf(
      "  - Working Set Hit Rate: %.2f%% (%zu cache hits out of %zu faults)\n",
      working_set_hit_rate * 100.0, total_faults - swap_ins, total_faults);
  printf("  - Swap-In Ratio:        %.2f%% (%zu remote fetches)\n",
         swap_in_ratio * 100.0, swap_ins);
  printf("  - Dirty Page Ratio:     %.2f%% (%zu write-backs / %zu evictions)\n",
         dirty_ratio * 100.0, swap_outs, total_evictions);
  printf("  - Cache Pollution:      %.2f%% (%zu discarded fresh pages)\n",
         pollution_rate * 100.0, discards);
  printf("  - Proactive Efficiency: %.2f%% (%zu proactive / %zu total "
         "evictions)\n",
         proactive_efficiency * 100.0, proactive_evictions, total_evictions);
  printf("  - Stall Rate:           %.2f%% (%zu reactive evictions)\n",
         stall_rate * 100.0, reactive_evictions);

  printf("\nREBALANCER ADAPTATION:\n");
  printf("  - Total Cycles:           %lu\n",
         stats_.rebalancer_stats.total_cycles);
  printf("  - Current Interval:       %lu ms (stable for %lu cycles)\n",
         stats_.rebalancer_stats.current_interval_ms,
         stats_.rebalancer_stats.current_stable_cycles);
  printf("  - Pressure Events:        %lu (decreased interval)\n",
         stats_.rebalancer_stats.pressure_events);
  printf("  - Relaxation Events:      %lu (increased interval)\n",
         stats_.rebalancer_stats.relaxation_events);
  printf("  - Time at MIN (%u ms):    %zu ms (%.1f%% of runtime)\n",
         MIN_INTERVAL_MS, stats_.rebalancer_stats.time_at_min_ms,
         (stats_.rebalancer_stats.total_cycles) > 0
             ? 100.0 * (double)stats_.rebalancer_stats.time_at_min_ms /
                   (double)(stats_.rebalancer_stats.total_cycles *
                            rebalance_interval_ms_)
             : 0);
  printf("  - Time at MAX (%u ms):  %zu ms (%.1f%% of runtime)\n",
         MAX_INTERVAL_MS, stats_.rebalancer_stats.time_at_max_ms,
         (stats_.rebalancer_stats.total_cycles) > 0
             ? 100.0 * (double)stats_.rebalancer_stats.time_at_max_ms /
                   (double)(stats_.rebalancer_stats.total_cycles *
                            rebalance_interval_ms_)
             : 0);

  // Analyze shard imbalance
  printf("\n%lu SHARDS LOAD DISTRIBUTION:\n", config.num_shards);

  std::vector<std::pair<usize, usize>> shard_faults;
  usize max_faults = 0, min_faults = ULONG_MAX;

  for (usize i = 0; i < config.num_shards; ++i) {
    usize faults = stats_.shard_stats[i].faults.load();
    shard_faults.push_back({i, faults});
    max_faults = std::max(max_faults, faults);
    min_faults = std::min(min_faults, faults);
  }

  std::sort(shard_faults.begin(), shard_faults.end(),
            [](auto &a, auto &b) { return a.second > b.second; });

  double imbalance =
      (min_faults > 0) ? (double)max_faults / (double)min_faults : 0.0;

  printf("  - Load Imbalance:       %.2fx (max/min faults per shard)\n",
         imbalance);

  printf("  - Hottest 5 Shards:\n");
  for (usize i = 0; i < std::min(5UL, shard_faults.size()); ++i) {
    usize idx = shard_faults[i].first;
    usize faults = shard_faults[i].second;
    double avg_active = stats_.shard_stats[idx].samples > 0
                            ? (double)stats_.shard_stats[idx].active_size_sum /
                                  (double)stats_.shard_stats[idx].samples
                            : 0;
    double avg_inactive =
        stats_.shard_stats[idx].samples > 0
            ? (double)stats_.shard_stats[idx].inactive_size_sum /
                  (double)stats_.shard_stats[idx].samples
            : 0;

    printf("    Shard %4zu: %8zu faults, avg %.1f active, %.1f inactive\n", idx,
           faults, avg_active, avg_inactive);
  }

  printf("  - Coldest 5 Shards:\n");
  for (usize i = 0; i < std::min(5UL, shard_faults.size()); ++i) {
    usize pos = shard_faults.size() - 1 - i;
    usize idx = shard_faults[pos].first;
    usize faults = shard_faults[pos].second;
    double avg_active = stats_.shard_stats[idx].samples > 0
                            ? (double)stats_.shard_stats[idx].active_size_sum /
                                  (double)stats_.shard_stats[idx].samples
                            : 0;
    double avg_inactive =
        stats_.shard_stats[idx].samples > 0
            ? (double)stats_.shard_stats[idx].inactive_size_sum /
                  (double)stats_.shard_stats[idx].samples
            : 0;

    printf("    Shard %4zu: %8zu faults, avg %.1f active, %.1f inactive\n", idx,
           faults, avg_active, avg_inactive);
  }

  printf("\nRAW COUNTERS:\n");
  printf("  - Total Faults:         %zu (demand zeros: %zu, swap-ins: %zu)\n",
         total_faults, stats_.demand_zeros.load(), swap_ins);
  printf("  - Total Evictions:      %zu\n", total_evictions);
  printf("    └─ Dirty (swap-out):  %zu\n", swap_outs);
  printf("    └─ Clean (skip):      %zu\n", skipped_writes);
  printf("    └─ Fresh (discard):   %zu\n", discards);
  printf("  - LRU Activity:         %zu promotions, %zu demotions\n",
         promotions, stats_.demotions.load());
  printf("  - Rebalancer Skips:     %zu\n", stats_.rebalancer_skips.load());
  printf("=================================================================\n");
}
