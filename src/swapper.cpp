#include <cstdio>
#include <mutex>
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

bool pte_is_present(u64 pte) { return pte & PTE_P; }
bool pte_is_writable(u64 pte) { return pte & PTE_W; }
bool pte_is_accessed(u64 pte) { return pte & PTE_A; }
bool pte_is_dirty(u64 pte) { return pte & PTE_D; }

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
    : storage_(std::move(storage)) {

  cache_base_addr_ = storage_->get_cache_base_addr();
  if (!cache_base_addr_) {
    PANIC(
        "Swapper initialized with a storage backend that has no cache address");
  }

  pages_ = std::make_unique<Page[]>(NUM_PAGES);
  for (size_t i = 0; i < NUM_PAGES; i++) {
    free_pages_.push_back(&pages_[i]);
  }

  INFO("Swapper initialized with %zu cache pages.", NUM_PAGES);
}

Swapper::~Swapper() { DEBUG("Destructing Swapper"); }

void Swapper::start_background_rebalancing() {
  rebalance_thread_ = std::thread([this]() {
    while (true) {
      sleep_ms(rebalance_interval_ms_);

      {
        std::unique_lock lock(pages_mutex_);
        if (lock.owns_lock()) {
          // Rebalance lists
          demote_cold_pages();
          promote_hot_pages();
          reap_cold_pages();
        } else {
          stats_.rebalancer_skips++;
        }
      }

      // Adapt the interval at which this thread runs
      adapt_rebalance_interval();
    }
  });

  rebalance_thread_.detach();
}

void Swapper::print_state(bool use_lock) {
  std::unique_ptr<std::lock_guard<std::mutex>> lock;
  if (use_lock) {
    lock = std::make_unique<std::lock_guard<std::mutex>>(pages_mutex_);
  }

  printf("Num pages: %zu -> Free: %zu, Inactive: %zu, Active: %zu\n", NUM_PAGES,
         free_pages_.size(), inactive_pages_.size(), active_pages_.size());

  printf("=== Free Pages (%zu) ===\n", free_pages_.size());
  for (auto p : free_pages_) {
    p->print();
  }

  printf("=== Inactive Pages (%zu) ===\n", inactive_pages_.size());
  for (auto p : inactive_pages_) {
    p->print();
  }

  printf("=== Active Pages (%zu) ===\n", active_pages_.size());
  for (auto p : active_pages_) {
    p->print();
  }

  printf("=== Mappings (%zu) ===\n", heap_state_map_.size());
  for (auto p : heap_state_map_) {
    printf("0x%lx -> %s\n", p.first, page_state_to_str(p.second));
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
  printf("  - Churn Rate:           %.2f%% (swap-ins per eviction)\n",
         churn_rate * 100.0);
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
  printf("    - Rebalancer skips: %zu\n", stats_.rebalancer_skips.load());
  printf("=================================================================\n");
}

usize Swapper::get_page_idx(Page *page) { return (usize)(page - pages_.get()); }

uptr Swapper::get_cache_gva(Page *page) {
  usize page_idx = get_page_idx(page);
  auto cache_offset = page_idx * PAGE_SIZE;
  return (uptr)cache_base_addr_ + cache_offset;
}

Page *Swapper::acquire_page() {
  if (!free_pages_.empty()) {
    auto page = free_pages_.front();
    free_pages_.pop_front();
    return page;
  }

  // no free page found, eviction is necessary
  Page *victim_page = nullptr;
  if (!inactive_pages_.empty()) {
    victim_page = inactive_pages_.back();
    inactive_pages_.pop_back();
    DEBUG("ACQUIRE_PAGE: Free list empty. Chose victim GVA 0x%lx from inactive "
          "list.",
          victim_page->vaddr);
  } else if (!active_pages_.empty()) {
    victim_page = active_pages_.back();
    active_pages_.pop_back();
    DEBUG("ACQUIRE_PAGE: Free list empty. Chose victim GVA 0x%lx from active "
          "list.",
          victim_page->vaddr);
  } else {
    UNREACHABLE("No pages available to evict from any list");
  }

  // evict chosen page
  swap_out_page(victim_page);

  // increase count of evictions to possibly
  // adapt the frequency of the rebalancing
  evictions_in_cycle_++;

  // increase related statistic counter
  stats_.reactive_evictions++;

  return victim_page;
}

void Swapper::swap_in_page(Page *page, uptr aligned_fault_vaddr) {
  auto target_offset = aligned_fault_vaddr - HEAP_START;
  auto cache_gva = get_cache_gva(page);
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);

  INFO("Swapping IN: 0x%lx into cache slot %zu. Cache (gva->gpa): 0x%lx->0x%lx",
       aligned_fault_vaddr, get_page_idx(page), cache_gva, cache_gpa);

  int ret = storage_->read_page((void *)cache_gva, target_offset);
  if (ret != 0) {
    ERROR("Failed to swap in: Backend read failed (%d)", ret);
    // NOTE: Robust error handling would be needed here.
    return;
  }

  map_gva(aligned_fault_vaddr, cache_gpa);
  page->vaddr = aligned_fault_vaddr;
}

void Swapper::swap_out_page(Page *page) {
  // victim page is the n-th page within the heap
  uptr victim_vaddr = page->vaddr;

  // FIX: the dirty-bit check needs to be implemented appropriately
  // if (auto perms = mapper_t::get_protect(victim_vaddr); pte_is_dirty(perms))
  // {
  auto victim_offset = victim_vaddr - HEAP_START;
  auto cache_gva = get_cache_gva(page);
  INFO("Swapping OUT: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx", victim_vaddr,
       cache_gva, mapper_t::gva_to_gpa((void *)cache_gva));

  storage_->write_page((void *)cache_gva, victim_offset);
  heap_state_map_[victim_vaddr] = PageState::RemotelyMapped;

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

  // print_state(false);
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

  std::lock_guard<std::mutex> lock(pages_mutex_);

  PageState page_state = heap_state_map_.contains(aligned_fault_vaddr)
                             ? heap_state_map_.at(aligned_fault_vaddr)
                             : PageState::Unmapped;

  Page *page = acquire_page();

  if (page_state == PageState::Unmapped) {
    DEBUG("Assigning page for gva 0x%lx", aligned_fault_vaddr);
    // TODO: this logic is almost identical to the swap-in,
    // but without the RDMA read. Refactor this.
    auto cache_gva = get_cache_gva(page);
    memset((void *)cache_gva, 0, PAGE_SIZE);
    auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_gva);
    map_gva(aligned_fault_vaddr, cache_gpa);
    page->vaddr = aligned_fault_vaddr;
    stats_.demand_zeros++;
  } else if (page_state == PageState::RemotelyMapped) {
    swap_in_page(page, aligned_fault_vaddr);
    stats_.swap_ins++;
  } else {
    UNREACHABLE("A %s page (0x%lx) shouldn't cause a fault.",
                page_state_to_str(page_state), aligned_fault_vaddr);
  }

  heap_state_map_[aligned_fault_vaddr] = PageState::Mapped;
  active_pages_.push_front(page);

  // print_state(false);
}

void Swapper::demote_cold_pages() {
  // std::lock_guard<std::mutex> lock(pages_mutex_);

  // iterate from the back (warm pages) to the front (hottest pages)
  for (auto it = active_pages_.rbegin(); it != active_pages_.rend();) {
    auto hot_page = *it;

    if (auto perms = mapper_t::get_protect(hot_page->vaddr);
        pte_is_accessed(perms)) {
      // page is still hot, clear the A-bit and keep it in the active list
      clear_permissions(hot_page->vaddr, PTE_A);
      ++it;
    } else {
      // page has become cold, demote it to the inactive list
      inactive_pages_.push_front(hot_page);

      // basically like calling `erase` but in reverse
      it = std::list<Page *>::reverse_iterator(
          active_pages_.erase(std::next(it).base()));

      stats_.demotions++;
    }
  }
}

void Swapper::promote_hot_pages() {
  // std::lock_guard<std::mutex> lock(pages_mutex_);

  for (auto it = inactive_pages_.begin(); it != inactive_pages_.end();) {
    auto cold_page = *it;

    if (auto perms = mapper_t::get_protect(cold_page->vaddr);
        pte_is_accessed(perms)) {
      // page has become hot, promote to active
      active_pages_.push_front(cold_page);
      it = inactive_pages_.erase(it);
      clear_permissions(cold_page->vaddr, PTE_A);

      stats_.promotions++;
    } else {
      // page is still cold, keep it in the inactive list
      ++it;
    }
  }
}

void Swapper::reap_cold_pages() {
  // std::lock_guard<std::mutex> lock(pages_mutex_);

  // try to keep some amount of free space in the cache
  while (free_pages_.size() < REAP_RESERVE && !inactive_pages_.empty()) {
    Page *victim_page = inactive_pages_.back();
    inactive_pages_.pop_back();

    swap_out_page(victim_page);
    free_pages_.push_back(victim_page);

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
    cycles_since_bad_event_ = 0;
  } else {
    cycles_since_bad_event_++;

    // stable system: additive increase
    if (cycles_since_bad_event_ >= COOLDOWN_CYCLES) {
      auto cur = rebalance_interval_ms_;
      u32 new_interval = cur + ADDITIVE_INCREASE_MS;
      rebalance_interval_ms_ = std::min(MAX_INTERVAL_MS, new_interval);

      INFO("[ADAPT] Stable system: increased rebalancing frequency to %u ms",
           rebalance_interval_ms_);

      // reset the counter to require another
      // full cooldown period before the next increase
      cycles_since_bad_event_ = 0;
    }

    // if the cooldown threshold wasn't reached,
    // maintain the current interval
  }
}
