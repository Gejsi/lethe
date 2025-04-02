#include <atomic>
#include <cstdlib>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <thread>
#include <time.h>

#include "volimem/idt.h"
#include "volimem/mapper.h"
#include "volimem/utils.h"
#include "volimem/volimem.h"

#define UNUSED(x) (void)(x)
#define START_ADDR 0xffff800000000000

constexpr size_t KB = 1024;
constexpr size_t MB = KB * KB;
constexpr size_t GB = MB * KB;

constexpr size_t PAGE_SIZE = 4 * KB;
constexpr size_t CACHE_SIZE = 128 * MB;
constexpr size_t NUM_PAGES = CACHE_SIZE / PAGE_SIZE;
constexpr size_t SWAP_SIZE = 10 * GB;

constexpr size_t SCAN_INTERVAL_MS = 1000;     // 1 second scan interval
constexpr uint64_t EVICT_THRESHOLD_MS = 2000; // 2 seconds eviction interval

struct Page {
  uint64_t vaddr;
  uint64_t last_scan;
  uint64_t last_access;
  bool in_cache;
  uint64_t swap_offset;
  uint32_t hotness;
};

// addr_in cache = cache + gpa(vadrr) - gpa (cache)

Page *pages; // page_index = (vaddr - cache_start) / page_size
uint8_t pages_mutex;

struct PageMeta {
  uint64_t last_scan;
  uint64_t last_access;
  bool is_swapped;
  size_t swap_offset;
};

void *cache_area; // client
void *swap_area;  // server
std::unordered_map<uint64_t, PageMeta> page_map;
uint8_t map_mutex;
std::atomic<bool> running{true};

uint64_t get_timestamp() {
  struct timespec timestamp;
  clock_gettime(CLOCK_MONOTONIC, &timestamp);
  return static_cast<uint64_t>(timestamp.tv_sec) * 1000 +
         static_cast<uint64_t>(timestamp.tv_nsec) / 1000000;
}

// revoke access periodically
void scan_thread() {
  while (running) {
    mutex_lock(&pages_mutex);
    uint64_t now = get_timestamp();

    for (size_t i = 0; i < NUM_PAGES; ++i) {
      if (pages[i].in_cache) {
        mapper_t::mprotect(pages[i].vaddr, PAGE_SIZE, PTE_NONE);
        pages[i].last_scan = now;
      }
    }

    mutex_unlock(&pages_mutex);
    std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_INTERVAL_MS));
  }
}

void evictor_thread() {
  while (running) {
    mutex_lock(&pages_mutex);

    uint32_t min_hotness = UINT32_MAX;
    size_t coldest_idx = NUM_PAGES;

    // find coldest page
    for (size_t i = 0; i < NUM_PAGES; ++i) {
      if (pages[i].in_cache && pages[i].hotness < min_hotness) {
        min_hotness = pages[i].hotness;
        coldest_idx = i;
      }
    }

    if (coldest_idx != NUM_PAGES) {
      void *phys = reinterpret_cast<void *>(mapper_t::gva_to_gpa(
          reinterpret_cast<void *>(pages[coldest_idx].vaddr)));

      memcpy((uint64_t *)swap_area + pages[coldest_idx].swap_offset, phys,
             PAGE_SIZE);
      mapper_t::mprotect(pages[coldest_idx].vaddr, PAGE_SIZE, PTE_NONE);
      pages[coldest_idx].in_cache = false;
      pages[coldest_idx].hotness = 0;
    }

    mutex_unlock(&pages_mutex);
    std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_INTERVAL_MS));
  }
}

void handle_faults(void *args) {
  regstate_t *state = static_cast<regstate_t *>(args);
  uint64_t fault_addr = state->fault_addr;
  // const uint64_t now = get_timestamp();
  // size_t page_idx =
  //     (mapper_t::gva_to_gpa(&fault_addr) - mapper_t::gva_to_gpa(cache_area))
  //     / PAGE_SIZE;

  mutex_lock(&pages_mutex);

  // for (size_t i = 0; i < NUM_PAGES; ++i) {
  //   if (pages[i].vaddr == fault_addr) {
  //     if (!pages[i].in_cache) {
  //       // swap-in
  //       void *phys = reinterpret_cast<void *>(
  //           mapper_t::gva_to_gpa(reinterpret_cast<void *>(fault_addr)));
  //       memcpy(phys, (uint64_t *)swap_area + pages[i].swap_offset,
  //       PAGE_SIZE); pages[i].in_cache = true;
  //     }

  //     if (pages[i].last_scan > 0) {
  //       const uint64_t delta = now - pages[i].last_scan;
  //       pages[i].hotness =
  //           (delta > 0) ? (SCAN_INTERVAL_MS * 1000 / delta) : UINT32_MAX;
  //     }

  //     pages[i].last_access = now;
  //     mapper_t::mprotect(fault_addr, PAGE_SIZE, PTE_P | PTE_W | PTE_U);
  //     break;
  //   }
  // }

  mutex_unlock(&pages_mutex);
}

void virtual_main(void *args) {
  UNUSED(args);

  segment_t *seg = new segment_t(SWAP_SIZE, START_ADDR);
  mapper_t::assign_handler(seg, handle_faults);

  std::thread scanner(scan_thread);
  std::thread evictor(evictor_thread);

  uint8_t *data = (uint8_t *)START_ADDR;
  for (size_t i = 0; i < SWAP_SIZE; i += PAGE_SIZE) {
    data[i] = i % 256; // trigger page faults
  }

  scanner.join();
  evictor.join();
}

constexpr s_volimem_config_t voli_config{
    .log_level = DEBUG,
    .host_page_type = VOLIMEM_NORMAL_PAGES,
    .guest_page_type = VOLIMEM_NORMAL_PAGES,
};

int main() {
  swap_area = mmap(nullptr, SWAP_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  cache_area = mmap(nullptr, CACHE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  pages = (Page *)calloc(NUM_PAGES, sizeof(Page));
  for (size_t i = 0; i < NUM_PAGES; ++i) {
    pages[i] = {.vaddr = (uint64_t)(START_ADDR + i * PAGE_SIZE),
                .last_scan = 0,
                .last_access = 0,
                .in_cache = true,
                .swap_offset = i * PAGE_SIZE,
                .hotness = 0};
  }

  volimem_set_config(&voli_config);
  volimem_start(nullptr, virtual_main);

  while (true) {
    // keep alive
  }
}
