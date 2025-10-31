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
      sleep_ms(200);
      demote_cold_pages();
      promote_hot_pages();
      reap_cold_pages();
    }
  });

  rebalance_thread_.detach();
}

void Swapper::print_state(const char *caller) {
  std::lock_guard<std::mutex> lock(pages_mutex_);

  printf("[%s] Free: %zu, Inactive: %zu, Active: %zu\n", caller,
         free_pages_.size(), inactive_pages_.size(), active_pages_.size());

  printf("=== Free Pages ===\n");
  for (auto p : free_pages_) {
    p->print();
  }

  printf("=== Inactive Pages ===\n");
  for (auto p : inactive_pages_) {
    p->print();
  }

  printf("=== Active Pages ===\n");
  for (auto p : active_pages_) {
    p->print();
  }

  printf("=== States ===\n");
  for (auto p : state_map_) {
    printf("0x%lx -> %s\n", p.first, page_state_to_str(p.second));
  }
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
  } else if (!active_pages_.empty()) {
    victim_page = active_pages_.back();
    active_pages_.pop_back();
  } else {
    UNREACHABLE("No pages available to evict from any list");
  }

  // evict chosen page
  swap_out_page(victim_page);

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

  if (auto perms = mapper_t::get_protect(victim_vaddr); pte_is_dirty(perms)) {
    auto victim_offset = victim_vaddr - HEAP_START;
    auto cache_gva = get_cache_gva(page);
    INFO("Swapping OUT: 0x%lx. Cache (gva->gpa): 0x%lx->0x%lx", victim_vaddr,
         cache_gva, mapper_t::gva_to_gpa((void *)cache_gva));

    storage_->write_page((void *)cache_gva, victim_offset);
    state_map_[victim_vaddr] = PageState::RemotelyMapped;
  } else {
    INFO("Page %zu (GVA 0x%lx) is CLEAN. Skipping write-back.",
         (size_t)(page - pages_.get()), victim_vaddr);

    state_map_[victim_vaddr] = PageState::Unmapped;
    // state_map_.erase(victim_vaddr);
  }

  unmap_gva(victim_vaddr);
  page->reset();
}

void Swapper::handle_fault(void *fault_addr, regstate_t *regstate) {
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

  std::lock_guard<std::mutex> lock(pages_mutex_);

  PageState page_state = state_map_.contains(aligned_fault_vaddr)
                             ? state_map_.at(aligned_fault_vaddr)
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
  } else if (page_state == PageState::RemotelyMapped) {
    swap_in_page(page, aligned_fault_vaddr);
  } else {
    UNREACHABLE("A %s page (0x%lx) shouldn't cause a fault.",
                page_state_to_str(page_state), aligned_fault_vaddr);
  }

  state_map_[aligned_fault_vaddr] = PageState::Mapped;
  active_pages_.push_front(page);
}

void Swapper::demote_cold_pages() {
  std::lock_guard<std::mutex> lock(pages_mutex_);
  DEBUG("----------START DEMOTE----------");

  for (auto it = active_pages_.begin(); it != active_pages_.end();) {
    auto hot_page = *it;

    if (auto perms = mapper_t::get_protect(hot_page->vaddr);
        pte_is_accessed(perms)) {
      // page is still hot, clear the A-bit and keep it in the active list
      clear_permissions(hot_page->vaddr, PTE_A);
      ++it;
      DEBUG("Page %zu is still hot, giving another chance",
            get_page_idx(hot_page));
    } else {
      // page has become cold, demote it to the inactive list
      inactive_pages_.push_back(hot_page);
      it = active_pages_.erase(it); // returns iterator to the next element
      DEBUG("Demoted page %zu (gva 0x%lx) to inactive list",
            get_page_idx(hot_page), hot_page->vaddr);
    }
  }

  DEBUG("----------END DEMOTE----------");
}

void Swapper::promote_hot_pages() {
  std::lock_guard<std::mutex> lock(pages_mutex_);
  DEBUG("----------START PROMOTE----------");

  for (auto it = inactive_pages_.begin(); it != inactive_pages_.end();) {
    auto cold_page = *it;

    if (auto perms = mapper_t::get_protect(cold_page->vaddr);
        pte_is_accessed(perms)) {
      // page has become hot, promote to active
      active_pages_.push_front(cold_page);
      it = inactive_pages_.erase(it);
      clear_permissions(cold_page->vaddr, PTE_A);
      DEBUG("Promoted page %zu (gva 0x%lx) to active list",
            get_page_idx(cold_page), cold_page->vaddr);
    } else {
      // page is still cold, keep it in the inactive list
      ++it;
      DEBUG("Page %zu is still cold", get_page_idx(cold_page));
    }
  }

  DEBUG("----------END PROMOTE----------");
}

void Swapper::reap_cold_pages() {
  std::lock_guard<std::mutex> lock(pages_mutex_);
  DEBUG("----------START REAP----------");

  while (free_pages_.size() < REAP_THRESHOLD && !inactive_pages_.empty()) {
    Page *victim_page = inactive_pages_.back();
    inactive_pages_.pop_back();

    uptr victim_gva = victim_page->vaddr;

    swap_out_page(victim_page);

    free_pages_.push_back(victim_page);

    DEBUG("Reaped page %zu (GVA 0x%lx) to maintain some amount of free pages",
          get_page_idx(victim_page), victim_gva);
  }
  DEBUG("----------END REAP----------");
}
