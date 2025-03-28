#include <atomic>
#include <cstdlib>
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

constexpr size_t KB = 1024;
constexpr size_t MB = KB * KB;
constexpr size_t GB = MB * KB;

constexpr size_t SCAN_INTERVAL_MS = 1000; // 1 second scan interval

// TODO: already defined in volimem
constexpr uint64_t VOLIMEM_GUEST_PAGE_SIZE = 2 * MB;

constexpr size_t CLIENT_MEM = 128 * MB;       // 128MB physical
constexpr size_t SERVER_SWAP = 1 * GB;        // 1GB swap
constexpr uint64_t EVICT_THRESHOLD_MS = 2000; // 2 seconds idle

struct PageMeta {
  uint64_t last_scan;
  uint64_t last_access;
  bool is_swapped;
  size_t swap_offset;
  // TODO: vaddr
};

void *swap_region;
std::unordered_map<uint64_t, PageMeta> page_map;
// TODO: (gva_to_gpa(vaddr) - start) >> 12
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
    mutex_lock(&map_mutex);
    uint64_t now = get_timestamp();

    for (auto &[vaddr, meta] : page_map) {
      if (!meta.is_swapped) {
        mapper_t::mprotect(vaddr, VOLIMEM_GUEST_PAGE_SIZE, PTE_NONE);
        meta.last_scan = now;
      }
    }

    mutex_unlock(&map_mutex);
    std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_INTERVAL_MS));
  }
}

void evictor_thread() {
  while (running) {
    mutex_lock(&map_mutex);
    uint64_t now = get_timestamp();
    size_t active_pages = 0;

    for (const auto &[_, meta] : page_map) {
      if (!meta.is_swapped)
        active_pages++;
    }

    // evict if over limit
    if (active_pages > (CLIENT_MEM / VOLIMEM_GUEST_PAGE_SIZE)) {
      uint64_t oldest_vaddr = 0;
      uint64_t oldest_time = UINT64_MAX;

      for (const auto &[vaddr, meta] : page_map) {
        if (!meta.is_swapped && meta.last_access < oldest_time) {
          oldest_time = meta.last_access;
          oldest_vaddr = vaddr;
        }
      }

      if (oldest_vaddr != 0 && (now - oldest_time) > EVICT_THRESHOLD_MS) {
        auto &meta = page_map[oldest_vaddr];
        void *phys_addr = reinterpret_cast<void *>(
            mapper_t::gva_to_gpa(reinterpret_cast<void *>(oldest_vaddr)));
        memcpy(static_cast<char *>(swap_region) + meta.swap_offset, phys_addr,
               VOLIMEM_GUEST_PAGE_SIZE);
        mapper_t::unmap(oldest_vaddr, VOLIMEM_GUEST_PAGE_SIZE);
        meta.is_swapped = true;
      }
    }
    mutex_unlock(&map_mutex);
    std::this_thread::sleep_for(std::chrono::milliseconds(SCAN_INTERVAL_MS));
  }
}

void fault_handler(void *args) {
  regstate_t *state = static_cast<regstate_t *>(args);
  uint64_t vaddr = state->fault_addr & ~(VOLIMEM_GUEST_PAGE_SIZE - 1);

  mutex_lock(&map_mutex);
  auto &meta = page_map[vaddr];
  uint64_t now = get_timestamp();

  if (meta.is_swapped) {
    // swap in
    void *new_page =
        aligned_alloc(VOLIMEM_GUEST_PAGE_SIZE, VOLIMEM_GUEST_PAGE_SIZE);
    memcpy(new_page, static_cast<char *>(swap_region) + meta.swap_offset,
           VOLIMEM_GUEST_PAGE_SIZE);
    // TODO: use gva_to_gpa
    mapper_t::map_gpt(vaddr, reinterpret_cast<uint64_t>(new_page),
                      VOLIMEM_GUEST_PAGE_SIZE, PTE_P | PTE_W);
    meta.is_swapped = false;
  } else {
    meta.last_access = now;
    mapper_t::mprotect(vaddr, VOLIMEM_GUEST_PAGE_SIZE, PTE_P | PTE_W);
  }

  mutex_unlock(&map_mutex);
}

void virtual_main(void *args) {
  UNUSED(args);

  segment_t *seg = new segment_t(0, PTE_P); // monitor entire address space
  mapper_t::assign_handler(seg, fault_handler);

  std::thread(scan_thread).detach();
  std::thread(evictor_thread).detach();
}

constexpr s_volimem_config_t voli_config{
    .log_level = DEBUG,
    .host_page_type = VOLIMEM_NORMAL_PAGES,
    .guest_page_type = VOLIMEM_NORMAL_PAGES,
};

/*
 * meta_entry_to_cache_addr(meta_page)
 * cpu_heap_to_mem_heap(..)
 *
 * gva_to_gdp
 *
 */
int main() {
  swap_region = mmap(nullptr, SERVER_SWAP, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

  volimem_set_config(&voli_config);
  volimem_start(nullptr, virtual_main);

  // allocate test memory (exceeds client limit)
  constexpr size_t TEST_SIZE = 256 * MB;
  void *test_buf = (void *)malloc(TEST_SIZE);
  memset(test_buf, 0, TEST_SIZE);

  // track pages
  uint64_t base = reinterpret_cast<uint64_t>(test_buf);
  size_t num_pages = TEST_SIZE / VOLIMEM_GUEST_PAGE_SIZE;
  mutex_lock(&map_mutex);
  for (size_t i = 0; i < num_pages; ++i) {
    page_map[base + i * VOLIMEM_GUEST_PAGE_SIZE] = {
        .last_scan = 0,
        .last_access = 0,
        .is_swapped = false,
        .swap_offset = i * VOLIMEM_GUEST_PAGE_SIZE,
    };
  }
  mutex_unlock(&map_mutex);

  while (true) {
    // keep alive
  }
}
