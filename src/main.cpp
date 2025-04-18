#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>

#include "swapper.h"
#include "volimem/mapper.h"
#include "volimem/volimem.h"

Page *pages;
void *cache_area; // client
void *swap_area;  // server
// std::unique_ptr<Swapper> swapper;

auto allocate_page() {
  void *new_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return mapper_t::gva_to_gpa(new_page);
}

usize find_victim() {
  static usize next_idx = 0;
  usize idx = next_idx;
  next_idx = (next_idx + 1) % NUM_PAGES;
  DEBUG("Filling cache slot %zu", idx);
  return idx;
}

void swap_out(usize victim_idx) {
  auto victim_vaddr = pages[victim_idx].vaddr;
  // victim page is the i-th page within the logical HEAP region
  auto victim_offset = victim_vaddr - HEAP_START;
  auto swap_dst = (uptr)swap_area + victim_offset;
  // auto cache_slot = (uptr)cache_area + victim_idx * PAGE_SIZE;
  if (victim_vaddr) {
    INFO("Swapping OUT victim: gva = 0x%lx, gpa = 0x%lx", victim_vaddr,
         mapper_t::gva_to_gpa((void *)victim_vaddr));
    memcpy((void *)swap_dst, (void *)victim_vaddr, PAGE_SIZE);
    mapper_t::unmap(victim_vaddr, PAGE_SIZE);
    // ensure that unmap actually invalidates the table entry
    mapper_t::flush(victim_vaddr, PAGE_SIZE);
    pages[victim_idx].vaddr = 0;
  }
}

void swap_in(usize victim_idx, uptr fault_addr) {
  auto cache_offset = victim_idx * PAGE_SIZE;
  auto cache_slot = (uptr)cache_area + cache_offset;
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_slot);
  auto aligned_fault_vaddr = fault_addr & ~(PAGE_SIZE - 1);
  INFO("Swapping IN: 0x%lx (aligned: 0x%lx), cache_gpa 0x%lx", fault_addr,
       aligned_fault_vaddr, cache_gpa);
  // map fault address to the cache slot's physical memory
  mapper_t::map_gpt(aligned_fault_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
  // copy data from swap to cache slot
  auto fault_offset = aligned_fault_vaddr - HEAP_START;
  auto swap_src = (uptr)swap_area + fault_offset;
  memcpy((void *)aligned_fault_vaddr, (void *)swap_src, PAGE_SIZE);
  pages[victim_idx].vaddr = aligned_fault_vaddr;
}

void handle_fault(void *fault_addr) {
  DEBUG("Inside fault handler: %p", fault_addr);

  auto victim_idx = find_victim();
  swap_out(victim_idx);
  swap_in(victim_idx, (uptr)fault_addr);
}

void virtual_main(void *args) {
  UNUSED(args);
  printf("--- Inside VM ---\n\n");

  auto seg = new segment_t(HEAP_SIZE, HEAP_START);
  mapper_t::assign_handler(seg, handle_fault);

  // for (size_t i = 0; i < 10; i++) {
  //   auto vaddr = HEAP_START + i * PAGE_SIZE;
  //   pages[i].vaddr = vaddr;
  //   auto gpa = allocate_page();
  //   printf("Page[%zu] gva=0x%lx, gpa = 0x%lx\n", i, vaddr, gpa);
  //   mapper_t::map_gpt(vaddr, gpa, PAGE_SIZE, PTE_P | PTE_W, std::nullopt,
  //                     false);
  // }

  // printf("\n");
  // {
  //   auto page = (uptr *)(HEAP_START + PAGE_SIZE * 4);
  //   printf("Attempting write to 0x%lx (page mapped: %s)\n", (uptr)page,
  //          mapper_t::is_mapped(page) ? "true" : "false");
  //   *page = 0x69;
  //   printf("Write succeeded: 0x%lx\n", *page);
  // }

  printf("\n");
  {
    auto page = (uptr *)(HEAP_START + PAGE_SIZE * 50);
    printf("Attempting write to 0x%lx (page mapped: %s)\n", (uptr)page,
           mapper_t::is_mapped(page) ? "true" : "false");
    *page = 0xDEADBEEF;
    printf("Write succeeded: 0x%lx\n", *page);
  }

  printf("\n\n");
  {
    auto page = (uptr *)(HEAP_START + PAGE_SIZE * 60);
    printf("Attempting write to 0x%lx (page mapped: %s)\n", (uptr)page,
           mapper_t::is_mapped(page) ? "true" : "false");
    *page = 0xCAFEBABE;
    printf("Write succeeded: 0x%lx\n", *page);
  }

  printf("\n\n");
  {
    auto page = (uptr *)(HEAP_START + PAGE_SIZE * 50);
    printf("Attempting write from 0x%lx (page mapped: %s)\n", (uptr)page,
           mapper_t::is_mapped(page) ? "true" : "false");
    printf("Read old content: 0x%lx\n", *page);
    *page = 0x777;
    printf("Write succeeded: 0x%lx\n", *page);
  }

  printf("\n--- Exiting VM ---\n");
}

int main() {
  cache_area = aligned_alloc(PAGE_SIZE, CACHE_SIZE);
  swap_area = aligned_alloc(PAGE_SIZE, SWAP_SIZE);
  pages = (Page *)malloc(NUM_PAGES * sizeof(Page));
  for (usize i = 0; i < NUM_PAGES; i++) {
    pages[i].vaddr = 0;
  }

  DEBUG("CACHE %p", cache_area);
  DEBUG("SWAP %p", swap_area);
  DEBUG("PAGES METADATA %p", (void *)pages);

  constexpr s_volimem_config_t voli_config{
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
  };

  volimem_set_config(&voli_config);
  return volimem_start(nullptr, virtual_main);
}
