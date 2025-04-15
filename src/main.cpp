#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#include "volimem/idt.h"
#include "volimem/mapper.h"
#include "volimem/utils.h"
#include "volimem/volimem.h"

#define HEAP_START 0xffff800000000000

#define UNUSED(x) (void)(x)
#define DEBUG(fmt, ...) printf("[DEBUG] " fmt "\n", ##__VA_ARGS__)
#define INFO(fmt, ...) printf("[INFO] " fmt "\n", ##__VA_ARGS__)
#define PANIC(fmt, ...)                                                        \
  do {                                                                         \
    fprintf(stderr, "\x1b[31m[PANIC] %s:%d (%s): " fmt "\x1b[0m\n", __FILE__,  \
            __LINE__, __func__, ##__VA_ARGS__);                                \
    abort();                                                                   \
  } while (0)

typedef uint8_t u8;
typedef uintptr_t uptr;

constexpr size_t KB = 1024;
constexpr size_t MB = KB * KB;
constexpr size_t GB = MB * KB;

constexpr size_t PAGE_SIZE = 4 * KB;
constexpr size_t CACHE_SIZE = 128 * MB;
constexpr size_t NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr size_t SWAP_SIZE = 1 * GB;
constexpr size_t HEAP_SIZE = SWAP_SIZE;

struct Page {
  uptr vaddr;
};

// addr_in cache = cache + gpa(vadrr) - gpa (cache)

Page *pages; // page_index = (vaddr - cache_start) / page_size
uint8_t pages_mutex;

void *cache_area; // client
void *swap_area;  // server

// TODO: add fancy LRU logic
size_t find_victim() { return 1; }

// void handler() {
//     // ...
//     // fault_addr: faulty address & -4096;

//     // suppose 1 single page in the cache_area
//     // cache_area => virtual adress of the cache content
//     // pages => virtual address of the cache meta data

//     int victim = find_victim(); // return 0

//     char* vaddr;
//     size_t off;
//     char* saddr;
//     uint64_t gpa;
//     // step 1: swap out the only page of the cache

//     vaddr = pages[victim].addr; // in the heap on the CPU node
//     voff = vaddr - START_ADDR;  // offset relative the beginning of the heap
//     saddr = swap_area + vaff;   // address in the swap
//     memcpy(saddr, vaddr, PAGE_SIZE);  // swap out

//     mapper_t::unmap(vaddr); // unmap

//     // step 2: swap in the page corresponding to fault_addr in victim
//     // fault_addr is the accessed virtual address on the CPU node
//     gpa = mapper_t::gva_to_gpa(cache_area + (victim << 12));
//     mapper_t::map(fault_addr, gpa, PROT_READ | PROT_WRITE);

//     voff = fault_addr - START_ADDR; // relative to beginning of the heap
//     saddr = swap_area + voff;

//     memcpy(fault_addr, swap_area, PAGE_SIZE);
// }

void handle_fault(void *addr) {
  DEBUG("Inside fault handler");
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
  auto victim_vaddr = pages[victim_idx].vaddr;    // in the heap on the CPU node
  auto victim_offset = victim_vaddr - HEAP_START; // offset from heap base
  auto swap_dst = (uptr)swap_area + victim_offset;
  DEBUG("swap_dst %p, victim_vaddr %p", (void *)swap_dst, (void *)victim_vaddr);
  if (victim_vaddr != 0) {
    INFO("Swapping OUT victim: gva = 0x%lx, gpa = 0x%lx", victim_vaddr,
         mapper_t::gva_to_gpa((void *)victim_vaddr));
    memcpy((void *)swap_dst, (void *)victim_vaddr, PAGE_SIZE);
    mapper_t::unmap(victim_vaddr, PAGE_SIZE);
    // ensure that unmap actually invalidates the table entry
    mapper_t::flush(victim_vaddr, PAGE_SIZE);
  }

  // --- SWAP IN PHASE ---
  auto cache_offset = victim_idx * PAGE_SIZE;
  auto cache_vaddr = (uptr)cache_area + cache_offset;
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_vaddr);
  auto aligned_fault_vaddr = (uptr)addr & ~(PAGE_SIZE - 1);
  INFO("Swapping IN: 0x%lx (aligned: 0x%lx), cache_gpa 0x%lx", (uptr)addr,
       aligned_fault_vaddr, cache_gpa);
  // map fault address to the cache slot's physical memory
  mapper_t::map_gpt(aligned_fault_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
  // copy data from swap to cache slot
  auto fault_offset = aligned_fault_vaddr - HEAP_START;
  auto swap_src = (uptr)swap_area + fault_offset;
  DEBUG("fault_vaddr %p, swap_src %p", (void *)swap_dst, (void *)swap_src);
  pages[victim_idx].vaddr = aligned_fault_vaddr;
  memcpy((void *)aligned_fault_vaddr, (void *)swap_src, PAGE_SIZE);
}

auto allocate_page() {
  void *new_page = mmap(nullptr, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return mapper_t::gva_to_gpa(new_page);
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
    // auto vaddr = HEAP_START + i * PAGE_SIZE;
    pages[i].vaddr = 0;
    // auto gpa = allocate_page();
    // printf("Page[%zu] gva=0x%lx, gpa = 0x%lx\n", i, vaddr, gpa);
    // mapper_t::map_gpt(vaddr, gpa, PAGE_SIZE, PTE_P | PTE_W, std::nullopt,
    //                   false);
  }

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
    printf("Attempting read from 0x%lx (page mapped: %s)\n", (uptr)page,
           mapper_t::is_mapped(page) ? "true" : "false");
    printf("Read again: 0x%lx\n", *page);
  }

  printf("\n--- Exiting VM ---\n");
}

int main() {
  cache_area = aligned_alloc(PAGE_SIZE, CACHE_SIZE);
  swap_area = aligned_alloc(PAGE_SIZE, SWAP_SIZE);
  pages = (Page *)malloc(NUM_PAGES * sizeof(Page));

  printf("CACHE %p\n", cache_area);
  printf("SWAP %p\n", swap_area);
  printf("PAGES METADATA %p\n", (void *)pages);

  constexpr s_volimem_config_t voli_config{
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
  };

  volimem_set_config(&voli_config);
  return volimem_start(nullptr, virtual_main);
}
