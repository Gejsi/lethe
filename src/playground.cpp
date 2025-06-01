#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <span>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "swapper.h"
// #include "tests/rr.cpp"
#include "utils.h"
#include "volimem/mapper.h"
#include "volimem/volimem.h"

Page *pages;
std::list<Page *> free_pages;
std::list<Page *> active_pages, inactive_pages; // mapped pages
void *cache_area;
void *swap_area;
// std::unique_ptr<Swapper> swapper;

auto allocate_page() {
  void *new_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return mapper_t::gva_to_gpa(new_page);
}

inline void sleep_ms(usize ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void trigger_write(usize page_idx, uptr value) {
  uptr vaddr = HEAP_START + PAGE_SIZE * page_idx;
  auto ptr = (uptr *)vaddr;

  printf("\nWrite to Page %zu\n", page_idx);
  printf("Attempting write access to %p (mapped: %s)\n", (void *)ptr,
         bool_to_str(mapper_t::is_mapped(ptr)));

  *ptr = value;
  printf("Write succeeded: 0x%lx\n", *ptr);
}

void trigger_read(usize page_idx) {
  uptr vaddr = HEAP_START + PAGE_SIZE * page_idx;
  auto ptr = (uptr *)vaddr;

  printf("\nRead from Page %zu\n", page_idx);
  printf("Attempting read access to %p (mapped: %s)\n", (void *)ptr,
         bool_to_str(mapper_t::is_mapped(ptr)));
  printf("Read succeeded: 0x%lx\n", *ptr);
}

void print_pages() {
  for (auto &page : std::span(pages, NUM_PAGES)) {
    page.print();
  }
}

void print_lists() {
  printf("=== Free Pages ===\n");
  for (Page *p : free_pages) {
    p->print();
  }

  printf("=== Inactive Pages ===\n");
  for (Page *p : inactive_pages) {
    p->print();
  }

  printf("=== Active Pages ===\n");
  for (Page *p : active_pages) {
    p->print();
  }
}

void set_permissions(uptr vaddr, uptr flags, bool flush = true) {
  mapper_t::mprotect(vaddr, PAGE_SIZE, flags);
  if (flush) {
    mapper_t::flush(vaddr, PAGE_SIZE);
  }
}

usize retrieve_rr_page() {
  static usize next_idx = 0;
  usize idx = next_idx;
  next_idx = (next_idx + 1) % NUM_PAGES;
  DEBUG("Round-robin cache slot selected: %zu", idx);
  return idx;
}

std::optional<usize> find_free_page() {
  for (usize i = 0; i < NUM_PAGES; i++) {
    if (pages[i].state == PageState::Free) {
      DEBUG("Free cache slot selected: %zu", i);
      return i;
    }
  }

  return std::nullopt;
}

void demote() {
  std::list<Page *> hot_pages;

  while (!active_pages.empty()) {
    Page *p = active_pages.back();
    active_pages.pop_back();

    auto pte = mapper_t::get_protect(p->vaddr);
    if (pte_is_accessed(pte)) {
      set_permissions(p->vaddr, pte & ~(uptr)(PTE_A | PTE_D));
      hot_pages.push_front(p);
    } else {
      inactive_pages.push_front(p);
    }
  }

  active_pages = std::move(hot_pages);
}

// O(1)
std::optional<usize> retrieve_free_page() {
  if (free_pages.empty()) {
    DEBUG("No free cache slots available");
    return std::nullopt;
  }

  Page *p = free_pages.front();
  free_pages.pop_front();
  usize idx = (usize)(p - pages); // index with pointer arithmetic
  DEBUG("Free cache slot selected: %zu", idx);
  return idx;
}

void swap_out(uptr victim_vaddr, uptr swap_dst) {
  // auto cache_slot = (uptr)cache_area + 0 * PAGE_SIZE;
  memcpy((void *)swap_dst, (void *)victim_vaddr, PAGE_SIZE);

  mapper_t::unmap(victim_vaddr, PAGE_SIZE);
  // ensure that unmap actually invalidates the table entry
  mapper_t::flush(victim_vaddr, PAGE_SIZE);
}

void swap_in(uptr target_vaddr, uptr swap_src, uptr cache_gpa) {
  mapper_t::map_gpt(target_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
  memcpy((void *)target_vaddr, (void *)swap_src, PAGE_SIZE);
}

void swap_out_page(usize victim_idx) {
  // victim page is the i-th page within the heap
  auto &victim_page = pages[victim_idx];

  ENSURE(victim_page.state != PageState::Free,
         "Swap-out shouldn't be called for a free page.");

  auto victim_vaddr = victim_page.vaddr;
  INFO("Swapping OUT victim: gva = 0x%lx, gpa = 0x%lx, state = %s",
       victim_vaddr, mapper_t::gva_to_gpa((void *)victim_vaddr),
       page_state_to_str(victim_page.state));

  auto victim_offset = victim_vaddr - HEAP_START;
  auto swap_dst = (uptr)swap_area + victim_offset;
  // copy data from cache slot to swap
  swap_out(victim_vaddr, swap_dst);

  victim_page.vaddr = 0;
  victim_page.state = PageState::Free;
  free_pages.push_back(&victim_page);
}

void swap_in_page(usize target_idx, uptr aligned_fault_vaddr) {
  auto cache_offset = target_idx * PAGE_SIZE;
  auto cache_slot = (uptr)cache_area + cache_offset;
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_slot);
  INFO("Swapping IN: 0x%lx, cache_gpa 0x%lx", aligned_fault_vaddr, cache_gpa);

  auto target_offset = aligned_fault_vaddr - HEAP_START;
  auto swap_src = (uptr)swap_area + target_offset;
  // copy data from swap to cache slot
  swap_in(aligned_fault_vaddr, swap_src, cache_gpa);

  auto &target_page = pages[target_idx];
  target_page.vaddr = aligned_fault_vaddr;
  target_page.state = PageState::Mapped;
  inactive_pages.push_back(&target_page);
  // O(n)
  auto it = std::find(free_pages.begin(), free_pages.end(), &target_page);
  if (it != free_pages.end()) {
    free_pages.erase(it);
  }
}

void handle_fault(void *fault_addr) {
  DEBUG("Inside fault handler: %p", fault_addr);
  auto aligned_fault_vaddr = (uptr)fault_addr & ~(PAGE_SIZE - 1);

  if (auto free_idx = retrieve_free_page()) {
    swap_in_page(*free_idx, aligned_fault_vaddr);
    return;
  }

  // try evicting a cold page from the inactive list
  while (!inactive_pages.empty()) {
    Page *victim_page = inactive_pages.back();
    inactive_pages.pop_back();

    auto pte = mapper_t::get_protect(victim_page->vaddr);
    if (pte_is_accessed(pte)) {
      // clear the accessed and dirty bits to check future accesses
      set_permissions(victim_page->vaddr, pte & ~(uptr)(PTE_A | PTE_D));
      active_pages.push_front(victim_page);
    } else {
      usize victim_idx = (usize)(victim_page - pages);
      swap_out_page(victim_idx);
      swap_in_page(victim_idx, aligned_fault_vaddr);
      return;
    }
  }

  // no evictable cold pages: evict from active list
  if (!active_pages.empty()) {
    Page *victim_page = active_pages.back();
    active_pages.pop_back();

    usize victim_idx = (usize)(victim_page - pages);
    swap_out_page(victim_idx);
    swap_in_page(victim_idx, aligned_fault_vaddr);
    return;
  }

  PANIC("No pages available to evict!");
}

void virtual_main(void *args) {
  UNUSED(args);
  printf("--- Inside VM ---\n\n");

  auto seg = new segment_t(HEAP_SIZE, HEAP_START);
  mapper_t::assign_handler(seg, handle_fault);

  // for (size_t i = 0; i < NUM_PAGES; i++) {
  //   auto vaddr = HEAP_START + i * PAGE_SIZE;
  //   pages[i].vaddr = vaddr;
  //   pages[i].state = PageState::Mapped;
  //   auto gpa = allocate_page();
  //   mapper_t::map_gpt(vaddr, gpa, PAGE_SIZE, PTE_P | PTE_W);
  // }
  for (usize i = 0; i < NUM_PAGES; i++) {
    auto vaddr = HEAP_START + i * PAGE_SIZE;
    u8 tmp = *((u8 *)vaddr); // read to trigger a fault
    UNUSED(tmp);
  }

  print_lists();

  trigger_write(60, 0xdead);

  print_lists();

  // TODO: add background thread for `demote`

  printf("\n--- Exiting VM ---\n");
}

int main() {
  cache_area = aligned_alloc(PAGE_SIZE, CACHE_SIZE);
  swap_area = aligned_alloc(PAGE_SIZE, SWAP_SIZE);
  pages = new Page[NUM_PAGES];
  for (usize i = 0; i < NUM_PAGES; i++) {
    free_pages.push_back(&pages[i]);
  }

  DEBUG("CACHE 0x%lx", (uptr)cache_area);
  DEBUG("SWAP 0x%lx", (uptr)swap_area);
  DEBUG("PAGES METADATA 0x%lx", (uptr)pages);

  constexpr s_volimem_config_t voli_config{
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
  };

  // std::thread t([] { std::cout << "Hello from a thread!\n"; });
  // t.join();
  // std::cout << "Back in main\n";

  volimem_set_config(&voli_config);
  return volimem_start(nullptr, virtual_main);
}
