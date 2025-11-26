#include <algorithm>
#include <cstdio>
#include <mutex>
#include <random>
#include <volimem/mapper.h>

#include "swapper.h"
#include "utils.h"

void set_permissions(uptr vaddr, u64 flags) {
  mapper_t::mpermit(vaddr, PAGE_SIZE, flags);
  mapper_t::flush(vaddr, PAGE_SIZE);
}

void clear_permissions(uptr vaddr, u64 flags) {
  auto perms = mapper_t::get_protect(vaddr);
  mapper_t::mpermit(vaddr, PAGE_SIZE, perms & ~flags);
  mapper_t::flush(vaddr, PAGE_SIZE);
}

constexpr bool pte_is_present(u64 pte) { return pte & PTE_P; }
constexpr bool pte_is_writable(u64 pte) { return pte & PTE_W; }
constexpr bool pte_is_accessed(u64 pte) { return pte & PTE_A; }
constexpr bool pte_is_dirty(u64 pte) { return pte & PTE_D; }

void map_gva(uptr gva, uptr gpa) {
  mapper_t::map_gpt(gva, gpa, PAGE_SIZE, PTE_P | PTE_W);
}

void unmap_gva(uptr gva) {
  // unmap the page from the guest page table
  mapper_t::unmap(gva, PAGE_SIZE);
  // ensure that unmap actually invalidates the TLB entry
  mapper_t::flush(gva, PAGE_SIZE);
}

Swapper::Swapper(std::unique_ptr<Storage> storage)
    : storage_(std::move(storage)), pages_(std::make_unique<Page[]>(NUM_PAGES)),
      shards_(NUM_SHARDS),
      page_states_(std::make_unique<std::atomic<PageState>[]>(NUM_HEAP_PAGES)) {

  cache_base_addr_ = storage_->get_cache_base_addr();
  if (!cache_base_addr_) {
    PANIC(
        "Swapper initialized with a storage backend that has no cache address");
  }

  for (size_t i = 0; i < NUM_PAGES; i++) {
    free_pages_queue_.enqueue(&pages_[i]);
  }

  for (size_t i = 0; i < NUM_HEAP_PAGES; ++i) {
    page_states_[i].store(PageState::Unmapped, std::memory_order_relaxed);
  }

  INFO("Swapper initialized with %zu cache pages.", NUM_PAGES);
}

Swapper::~Swapper() { DEBUG("Destructing Swapper"); }

void Swapper::launch_background_rebalancing() {
  std::thread rebalance_thread = std::thread([this]() {
    std::vector<size_t> shard_indices(NUM_SHARDS);
    for (size_t i = 0; i < NUM_SHARDS; ++i)
      shard_indices[i] = i;

    std::random_device rd;
    std::default_random_engine rng(rd());

    while (true) {
      sleep_ms(rebalance_interval_ms_);

      // Shuffle the order of shards to visit
      std::shuffle(shard_indices.begin(), shard_indices.end(), rng);

      for (usize shard_idx : shard_indices) {

        Shard &shard = shards_[shard_idx];

        if (shard.mutex.try_lock()) {
          // Rebalance lists
          demote_cold_pages(shard);
          promote_hot_pages(shard);
          reap_cold_pages(shard);

          shard.mutex.unlock();
        } else {
          stats_.rebalancer_skips++;
        }
      }

      // Adapt the interval at which this thread runs
      adapt_rebalance_interval();
    }
  });

  rebalance_thread.detach();
}

void Swapper::print_state() {
  // printf("Num pages: %zu -> Free: %zu, Inactive: %zu, Active: %zu\n",
  // NUM_PAGES,
  //        free_pages_.size(), inactive_pages_.size(), active_pages_.size());

  // printf("=== Free Pages (%zu) ===\n", free_pages_.size());
  // for (auto p : free_pages_) {
  //   p->print();
  // }

  // printf("=== Inactive Pages (%zu) ===\n", inactive_pages_.size());
  // for (auto p : inactive_pages_) {
  //   p->print();
  // }

  // printf("=== Active Pages (%zu) ===\n", active_pages_.size());
  // for (auto p : active_pages_) {
  //   p->print();
  // }

  size_t mapped_count = 0;
  for (size_t i = 0; i < NUM_HEAP_PAGES; ++i) {
    if (page_states_[i].load() != PageState::Unmapped) {
      mapped_count++;
    }
  }
  printf("=== Mappings (%zu) ===\n", mapped_count);
  for (size_t i = 0; i < NUM_HEAP_PAGES; ++i) {
    PageState state = page_states_[i].load();
    if (state != PageState::Unmapped) {
      uptr vaddr = HEAP_START + (i * PAGE_SIZE);
      printf("0x%lx -> %s\n", vaddr, page_state_to_str(state));
    }
  }
}

void Swapper::print_stats() {
  usize total_faults = stats_.total_faults.load();
  usize swap_ins = stats_.swap_ins.load();
  usize swap_outs = stats_.swap_outs.load();
  usize clean_evictions = stats_.clean_evictions.load();
  usize reactive_evictions = stats_.reactive_evictions.load();
  usize proactive_evictions = stats_.proactive_evictions.load();
  usize promotions = stats_.promotions.load();

  // total evictions, categorized by content (dirty/clean)
  usize total_evictions_by_content = swap_outs + clean_evictions;
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
  printf("  - Total Page Faults:   %zu\n", total_faults);
  printf("    - Demand Zeros:    %zu\n", stats_.demand_zeros.load());
  printf("    - Swap-Ins:        %zu\n", swap_ins);
  printf("  - Total Evictions:     %zu\n", total_evictions_by_content);
  printf("    - Swap-Outs (Dirty): %zu\n", swap_outs);
  printf("    - Discards (Clean):  %zu\n", clean_evictions);
  printf("  - Eviction Triggers:\n");
  printf("    - Proactive (Reaper): %zu\n", proactive_evictions);
  printf("    - Reactive (Stall):   %zu\n", reactive_evictions);
  printf("  - LRU List Activity:\n");
  printf("    - Promotions:       %zu\n", promotions);
  printf("    - Demotions:        %zu\n", stats_.demotions.load());
  printf("    - Rebalancer skips:        %zu\n",
         stats_.rebalancer_skips.load());
  printf("=================================================================\n");
}

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
  return (vaddr / PAGE_SIZE) % NUM_SHARDS;
}

Page *Swapper::acquire_page(Shard &shard) {
  Page *victim_page = nullptr;

  // if (!free_pages_queue_.try_dequeue(victim_page)) {
  //   // victim_page = shard.free_pages.front();
  //   // shard.free_pages.pop_front();
  //   return victim_page;
  // }

  if (free_pages_queue_.try_dequeue(victim_page)) {
    return victim_page;
  }

  // {
  //   std::lock_guard<std::mutex> lock(shard.mutex);
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
  // }

  // increase count of evictions to possibly
  // adapt the frequency of the rebalancing
  evictions_in_cycle_++;

  // increase related statistic counter
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
    stats_.demand_zeros++;
  } else if (page_state == PageState::RemotelyMapped) {
    INFO("Swapping IN: 0x%lx into cache slot %zu. Cache (gva->gpa): "
         "0x%lx->0x%lx",
         aligned_fault_vaddr, get_page_idx(page), cache_gva, cache_gpa);

    auto target_offset = aligned_fault_vaddr - HEAP_START;
    int ret = storage_->read_page((void *)cache_gva, target_offset);
    if (ret != 0) {
      ERROR("Failed to swap in: Backend read failed (%d)", ret);
      // NOTE: Robust error handling would be needed here.
      return;
    }

    stats_.swap_ins++;
  } else {
    UNREACHABLE(
        "A %s page (0x%lx) shouldn't cause a fault that needs a swap-in",
        page_state_to_str(page_state), aligned_fault_vaddr);
  }

  page_states_[heap_idx] = PageState::Mapped;
  map_gva(aligned_fault_vaddr, cache_gpa);
  page->vaddr = aligned_fault_vaddr;
}

void Swapper::swap_out_page(Page *page) {
  // victim page is the n-th page within the heap
  uptr victim_vaddr = page->vaddr;

  // TODO: Remove this line. This is mostly used for debugging,
  // as it catches unexpected race conditions that cause a
  // mapped page to be selected for a swap-in
  clear_permissions(victim_vaddr, PTE_P | PTE_W);

  // FIX: the dirty-bit check needs to be implemented appropriately
  // if (auto perms = mapper_t::get_protect(victim_vaddr); pte_is_dirty(perms))
  // {
  auto victim_offset = victim_vaddr - HEAP_START;
  auto cache_gva = get_cache_gva(page);
  INFO("Swapping OUT: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx", victim_vaddr,
       cache_gva, mapper_t::gva_to_gpa((void *)cache_gva));

  storage_->write_page((void *)cache_gva, victim_offset);
  page_states_[get_heap_idx(victim_vaddr)] = PageState::RemotelyMapped;

  stats_.swap_outs++;
  /*
  } else {
    INFO("Page %zu (GVA 0x%lx) is CLEAN. Skipping write-back.",
         (size_t)(page - pages_.get()), victim_vaddr);

    heap_state_map_[victim_vaddr] = PageState::Unmapped;
    // heap_state_map_.erase(victim_vaddr);
    stats_.clean_evictions++;
  }
  */

  unmap_gva(victim_vaddr);
  page->reset();
}

void Swapper::handle_fault(void *fault_addr, regstate_t *regstate) {
  UNUSED(regstate);

  DEBUG("Handling fault at %p. Error code: %lu", fault_addr,
        regstate->error_code);

  // align to page boundary
  uptr aligned_fault_vaddr = (uptr)fault_addr & ~(PAGE_SIZE - 1);

  // faulting address should be within the heap
  if (aligned_fault_vaddr < HEAP_START ||
      aligned_fault_vaddr >= HEAP_START + HEAP_SIZE) {
    ERROR("Fault address 0x%lx outside of heap range [0x%lx, 0x%lx)",
          aligned_fault_vaddr, HEAP_START, HEAP_START + HEAP_SIZE);
    return;
  }

  stats_.total_faults++;

  usize shard_idx = get_shard_idx(aligned_fault_vaddr);
  Shard &shard = shards_[shard_idx];

  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    Page *page = acquire_page(shard);

    swap_in_page(page, aligned_fault_vaddr);
    shard.active_pages.push_front(page);
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
      // shard.active_pages.splice(shard.active_pages.begin(),
      // shard.active_pages,
      //                           std::next(it).base());

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
      // it = std::list<Page *>::reverse_iterator(
      //     shard.inactive_pages.erase(std::next(it).base()));
      clear_permissions(cold_page->vaddr, PTE_A);

      stats_.promotions++;
    } else {
      // page is still cold, keep it in the inactive list
      ++it;
    }
  }
}

void Swapper::reap_cold_pages(Shard &shard) {
  if (free_pages_queue_.size_approx() >= REAP_RESERVE) {
    return;
  }

  usize evictions = 0;

  // try to keep some amount of free space in the shard
  while (evictions < SHARD_REAP_RESERVE && !shard.inactive_pages.empty()) {
    Page *victim_page = shard.inactive_pages.back();
    shard.inactive_pages.pop_back();

    swap_out_page(victim_page);

    // shard.mutex.unlock();

    free_pages_queue_.enqueue(victim_page);
    evictions++;
    stats_.proactive_evictions++;

    // shard.mutex.lock();
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
