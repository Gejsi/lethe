#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "volimem/idt.h"
#include "volimem/mapper.h"
#include "volimem/utils.h"
#include "volimem/volimem.h"

#define UNUSED(x) (void)(x)
#define HEAP_START 0xffff800000000000

constexpr size_t KB = 1024;
constexpr size_t MB = KB * KB;
constexpr size_t GB = MB * KB;

constexpr size_t PAGE_SIZE = 4 * KB;
constexpr size_t CACHE_SIZE = 128 * MB;
constexpr size_t NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr size_t SWAP_SIZE = 1 * GB;
constexpr size_t HEAP_SIZE = SWAP_SIZE;

struct Page {
  uintptr_t vaddr;
};

// addr_in cache = cache + gpa(vadrr) - gpa (cache)

Page *pages; // page_index = (vaddr - cache_start) / page_size
uint8_t pages_mutex;

void *cache_area; // client
void *swap_area;  // server

// TODO: add fancy LRU logic
size_t find_victim() { return 1; }

void handle_fault(void *addr) {
  printf("\n--- Inside fault handler ---\n");
  // // page-aligned virtual address
  // uint64_t fault_vaddr = (uint64_t)(addr) & ~(PAGE_SIZE - 1);
  // printf("Faulting address: 0x%lx\n", fault_vaddr);

  // // map new page to resolve fault
  // void *new_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
  //                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  // uint64_t gpa = mapper_t::gva_to_gpa(new_page);

  // printf("Mapping 0x%lx (GVA) -> 0x%lx (GPA)\n", fault_vaddr, gpa);
  // mapper_t::map_gpt(fault_vaddr, gpa, PAGE_SIZE, PTE_P | PTE_W);

  // --- SWAP OUT PHASE ---
  auto victim_idx = find_victim();
  auto victim_vaddr = pages[victim_idx].vaddr; // in the heap on the CPU node
  printf("Swapping OUT victim: 0x%lx\n", victim_vaddr);
  auto victim_offset = victim_vaddr - HEAP_START; // offset from heap base
  auto swap_dst = (uintptr_t)swap_area + victim_offset;
  auto cache_offset = victim_idx * PAGE_SIZE;
  auto cache_vaddr = (uintptr_t)cache_area + cache_offset;
  printf("swap_dst %p, cache_vaddr %p\n", (void *)swap_dst,
         (void *)cache_vaddr);
  mapper_t::unmap(victim_vaddr, PAGE_SIZE);

  // --- SWAP IN PHASE ---
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_vaddr);
  auto aligned_fault_vaddr = (uintptr_t)addr & ~(PAGE_SIZE - 1);
  printf("Swapping IN: 0x%lx (aligned: 0x%lx)\n", (uintptr_t)addr,
         aligned_fault_vaddr);
  // map fault address to the cache slot's physical memory
  mapper_t::map_gpt(aligned_fault_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
  // copy data from swap to cache slot
  auto fault_offset = aligned_fault_vaddr - HEAP_START;
  auto swap_src = (uintptr_t)swap_area + fault_offset;

  // printf("swap_src = %p, aligned_fault_vaddr = %p\n", (void *)swap_src,
  //        (void *)aligned_fault_vaddr);
  // volatile uint8_t dummy = *(uint8_t *)swap_src;

  memcpy((void *)aligned_fault_vaddr, (void *)swap_src, PAGE_SIZE);
}

void virtual_main(void *args) {
  UNUSED(args);
  printf("--- Inside VM ---\n\n");

  auto seg = new segment_t(HEAP_SIZE, HEAP_START);
  mapper_t::assign_handler(seg, handle_fault);

  // trigger the handler
  // uintptr_t *test_ptr = (uintptr_t *)(HEAP_START + 0x1000);
  // printf("Attempting write to 0x%lx\n", (uintptr_t)test_ptr);
  // *test_ptr = 0xDEADBEEF;
  // printf("Write succeeded! Value: 0x%lx\n", *test_ptr);

  for (size_t i = 0; i < 10; i++) {
    auto vaddr = HEAP_START + i * PAGE_SIZE;
    pages[i].vaddr = vaddr;
    void *new_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t gpa = mapper_t::gva_to_gpa(new_page);
    mapper_t::map_gpt(vaddr, gpa, PAGE_SIZE, PTE_P | PTE_W);
  }

  auto page1 = (uintptr_t *)(HEAP_START + PAGE_SIZE);
  printf("Attempting write to 0x%lx\n", (uintptr_t)page1);
  *page1 = 0xDEADBEEF;
  printf("Write succeeded! Value: 0x%lx\n", *page1);

  printf("\n--- Exiting VM ---\n");
}

constexpr s_volimem_config_t voli_config{
    .log_level = DEBUG,
    .host_page_type = VOLIMEM_NORMAL_PAGES,
    .guest_page_type = VOLIMEM_NORMAL_PAGES,
};

int main() {
  // pages = (Page *)HEAP_START;
  cache_area = malloc(CACHE_SIZE);
  swap_area = malloc(SWAP_SIZE);

  pages = (Page *)malloc(NUM_PAGES * sizeof(Page));

  printf("CACHE %p\n", cache_area);
  printf("SWAP %p\n", swap_area);
  printf("PAGES METADATA %p\n", (void *)pages);

  volimem_set_config(&voli_config);
  return volimem_start(nullptr, virtual_main);
}
