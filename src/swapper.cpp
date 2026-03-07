#include <cstdio>
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
      page_states_(
          std::make_unique<std::atomic<PageState>[]>(config.num_heap_pages)) {
  free_pages_.reserve(config.num_pages);
  for (usize i = 0; i < config.num_pages; i++) {
    free_pages_.push_back(&pages_[i]);
  }

  INFO("Swapper initialized with %zu cache slots", config.num_pages);
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

  if (free_pages_.empty()) {
    direct_reclaim(RECLAIM_BATCH);
  }

  Page *page = free_pages_.back();
  free_pages_.pop_back();

  swap_in_page(page, aligned_fault_vaddr);
  inactive_pages_.push_front(page);
}

void Swapper::direct_reclaim(usize nr_to_reclaim) {
  usize reclaimed = 0;

  // Refill inactive list from active if needed
  if (inactive_pages_.empty()) {
    shrink_active_list();
  }

  // Evict from inactive (primary reclaim path)
  reclaimed += shrink_inactive_list(nr_to_reclaim);

  // Fallback: if inactive exhausted, demote more from active and retry
  if (reclaimed < nr_to_reclaim && !active_pages_.empty()) {
    shrink_active_list();
    reclaimed += shrink_inactive_list(nr_to_reclaim - reclaimed);
  }

  // Last resort: evict directly from active tail
  while (reclaimed < nr_to_reclaim && !active_pages_.empty()) {
    Page *victim = active_pages_.back();
    active_pages_.pop_back();
    swap_out_page(victim);
    free_pages_.push_back(victim);
    reclaimed++;
    stats_.reclaim_evictions++;
  }
}

usize Swapper::shrink_inactive_list(usize nr_to_reclaim) {
  usize reclaimed = 0;

  auto it = inactive_pages_.end();
  while (it != inactive_pages_.begin() && reclaimed < nr_to_reclaim) {
    --it;
    Page *page = *it;

    auto perms = mapper_t::get_protect(page->vaddr);

    if (pte_is_accessed(perms)) {
      // Page was re-accessed while on inactive: promote to active
      clear_permissions(page->vaddr, PTE_A);
      active_pages_.push_front(page);
      it = inactive_pages_.erase(it);
      stats_.promotions++;
    } else {
      // Evict cold page
      it = inactive_pages_.erase(it);
      swap_out_page(page);
      free_pages_.push_back(page);
      reclaimed++;
      stats_.reclaim_evictions++;
    }
  }

  return reclaimed;
}

void Swapper::shrink_active_list() {
  auto it = active_pages_.end();
  while (it != active_pages_.begin()) {
    --it;
    Page *page = *it;

    auto perms = mapper_t::get_protect(page->vaddr);

    if (pte_is_accessed(perms)) {
      // Second chance: clear A-bit, move to front
      clear_permissions(page->vaddr, PTE_A);
      active_pages_.splice(active_pages_.begin(), active_pages_, it);
      // iterator invalidated by splice, reset
      it = active_pages_.end();
    } else {
      // Cold: demote to inactive head
      inactive_pages_.push_front(page);
      it = active_pages_.erase(it);
      stats_.demotions++;
    }
  }
}

void Swapper::print_stats() {
  usize total_faults = stats_.total_faults;
  usize swap_ins = stats_.swap_ins;
  usize swap_outs = stats_.swap_outs;
  usize skipped_writes = stats_.skipped_writes;
  usize discards = stats_.discards;
  usize reclaim_evictions = stats_.reclaim_evictions;
  usize promotions = stats_.promotions;

  usize total_evictions = swap_outs + skipped_writes + discards;

  // Calculate metrics
  double swap_in_ratio =
      (total_faults > 0) ? (double)swap_ins / (double)total_faults : 0.0;
  double working_set_hit_rate = 1.0 - swap_in_ratio;
  double dirty_ratio =
      (total_evictions > 0) ? (double)swap_outs / (double)total_evictions : 0.0;
  double pollution_rate =
      (total_evictions > 0) ? (double)discards / (double)total_evictions : 0.0;

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

  printf("\nRAW COUNTERS:\n");
  printf("  - Total Faults:         %zu (demand zeros: %zu, swap-ins: %zu)\n",
         total_faults, stats_.demand_zeros, swap_ins);
  printf("  - Total Evictions:      %zu\n", total_evictions);
  printf("    └─ Dirty (swap-out):  %zu\n", swap_outs);
  printf("    └─ Clean (skip):      %zu\n", skipped_writes);
  printf("    └─ Fresh (discard):   %zu\n", discards);
  printf("  - Reclaim Evictions:    %zu\n", reclaim_evictions);
  printf("  - LRU Activity:         %zu promotions, %zu demotions\n",
         promotions, stats_.demotions);
  printf("  - Active Pages:         %zu\n", active_pages_.size());
  printf("  - Inactive Pages:       %zu\n", inactive_pages_.size());
  printf("  - Free Pages:           %zu\n", free_pages_.size());
  printf("=================================================================\n");
}
