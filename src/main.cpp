#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "swapper.h"
#include "tests/rr.cpp"
#include "volimem/mapper.h"
#include "volimem/volimem.h"

Page *pages;
void *cache_area; // client
void *swap_area;  // server
usize free_pages_count = 0;
// std::unique_ptr<Swapper> swapper;

/**
 * @param sample_fraction Percentage of pages to probe (e.g., 0.01 for 1%)
 */
// void perform_probing_scan(double sample_fraction) {
//     if (NUM_PAGES == 0 || sample_fraction <= 0.0) {
//         return;
//     }

//     INFO("Starting probing scan (sampling %.2f%%)...", sample_fraction *
//     100.0); auto now = get_current_timestamp(); usize pages_to_probe =
//     static_cast<usize>(NUM_PAGES * sample_fraction); if (pages_to_probe == 0)
//     pages_to_probe = 1; // Probe at least one if possible

//     // Simple random sampling (can be improved)
//     std::vector<usize> indices(NUM_PAGES);
//     std::iota(indices.begin(), indices.end(), 0); // Fill with 0, 1, ...,
//     NUM_PAGES-1 std::random_device rd; std::mt19937 g(rd());
//     std::shuffle(indices.begin(), indices.end(), g);

//     usize probed_count = 0;
//     for (usize i = 0; i < NUM_PAGES && probed_count < pages_to_probe; ++i) {
//         usize cache_idx = indices[i];
//         uptr target_vaddr = pages[cache_idx].vaddr;

//         // Only probe pages that are currently valid and mapped
//         if (target_vaddr != 0 && mapper_t::is_mapped((void*)target_vaddr)) {
//             DEBUG("Probing cache slot %zu (VAddr 0x%lx)", cache_idx,
//             target_vaddr);

//             set_permissions(PTE_NONE);

//             // Update metadata
//             pages[cache_idx].is_probed = true;
//             pages[cache_idx].scan_timestamp = now;
//             probed_count++;
//         }
//     }
//     INFO("Probing scan finished. Marked %zu pages as inaccessible.",
//     probed_count);
// }

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

usize find_rr_page() {
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

  DEBUG("No free cache slots available");
  return std::nullopt;
}

usize find_lru_page() {
  usize idx = 0;
  TimePoint oldest = TimePoint::max();
  bool found = false; // track if we found at least one mapped page

  for (usize i = 0; i < NUM_PAGES; i++) {
    if (pages[i].state != PageState::Free) {
      found = true;

      if (pages[i].last_fault < oldest) {
        oldest = pages[i].last_fault;
        idx = i;
      }
    }
  }

  // if the cache is full, we must have found at least one candidate
  ENSURE(found, "LRU didn't find any mapped pages");

  DEBUG("LRU cache slot selected: %zu", idx);
  return idx;
}

std::optional<usize> is_page_probed(uptr target_vaddr) {
  for (usize i = 0; i < NUM_PAGES; i++) {
    if (pages[i].vaddr == target_vaddr && pages[i].state == PageState::Probed) {
      return i;
    }
  }

  return std::nullopt;
}

inline bool pte_is_present(uptr pte) { return pte & PTE_P; }
inline bool pte_is_writable(uptr pte) { return pte & PTE_W; }
inline bool pte_is_accessed(uptr pte) { return pte & PTE_A; }
inline bool pte_is_dirty(uptr pte) { return pte & PTE_D; }

void set_permissions(uptr vaddr, uptr flags, bool flush = true) {
  mapper_t::mprotect(vaddr, PAGE_SIZE, flags);
  if (flush) {
    mapper_t::flush(vaddr, PAGE_SIZE);
  }
}

void mark() {
  usize new_probed_stat = 0;
  usize old_probed_stat = 0;
  usize free_stat = 0;

  for (auto &page : std::span(pages, NUM_PAGES)) {
    switch (page.state) {
    case PageState::Mapped:
      // update protection level to cause a fault in case of read/write
      set_permissions(page.vaddr, PTE_NONE, false);
      page.last_scan = Clock::now();
      page.state = PageState::Probed;
      new_probed_stat++;
      break;
    case PageState::Probed:
      old_probed_stat++;
      break;
    case PageState::Free:
      free_stat++;
      break;
    }
  }

  // flush all instead of flushing individual pages
  // when setting permissions, because it's more efficient
  if (new_probed_stat > 0) {
    mapper_t::flush_all();
  }

  DEBUG("Marked pages: newly probed=%zu, already probed=%zu, free=%zu",
        new_probed_stat, old_probed_stat, free_stat);
}

void swap_out(uptr victim_vaddr, uptr swap_dst) {
  // auto cache_slot = (uptr)cache_area + 0 * PAGE_SIZE;
  memcpy((void *)swap_dst, (void *)victim_vaddr, PAGE_SIZE);

  mapper_t::unmap(victim_vaddr, PAGE_SIZE);
  // ensure that unmap actually invalidates the table entry
  mapper_t::flush(victim_vaddr, PAGE_SIZE);
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

  if (victim_page.state == PageState::Probed) {
    DEBUG("Restoring access permissions before swappping out 0x%lx",
          victim_vaddr);
    set_permissions(victim_vaddr, PTE_P | PTE_W | PTE_U);
  }

  auto victim_offset = victim_vaddr - HEAP_START;
  auto swap_dst = (uptr)swap_area + victim_offset;
  // copy data from cache slot to swap
  swap_out(victim_vaddr, swap_dst);

  victim_page.vaddr = 0;
  victim_page.state = PageState::Free;
  victim_page.last_fault = {};
  victim_page.last_scan = {};
  victim_page.cit = {};
}

void swap_in(uptr target_vaddr, uptr swap_src, uptr cache_gpa) {
  mapper_t::map_gpt(target_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
  memcpy((void *)target_vaddr, (void *)swap_src, PAGE_SIZE);
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
  target_page.last_fault = Clock::now();
  target_page.last_scan = {};
  target_page.cit = {};
}

void handle_fault(void *fault_addr) {
  DEBUG("Inside fault handler: %p", fault_addr);
  auto aligned_fault_vaddr = (uptr)fault_addr & ~(PAGE_SIZE - 1);

  // TODO: create a map<vaddr, idx> to more efficiently check?
  if (auto probed_idx = is_page_probed(aligned_fault_vaddr)) {
    DEBUG("Restoring access permissions for 0x%lx", aligned_fault_vaddr);
    set_permissions(aligned_fault_vaddr, PTE_P | PTE_W | PTE_U);

    auto &probed_page = pages[*probed_idx];
    probed_page.state = PageState::Mapped;
    probed_page.last_fault = Clock::now();
    probed_page.cit = (probed_page.last_scan == TimePoint{})
                          ? Milliseconds{}
                          : std::chrono::duration_cast<Milliseconds>(
                                Clock::now() - probed_page.last_scan);

    return;
  }

  // TODO: add queue of free pages?
  if (auto free_idx = find_free_page()) {
    swap_in_page(*free_idx, aligned_fault_vaddr);
  } else {
    auto lru_idx = find_lru_page();
    swap_out_page(lru_idx);
    swap_in_page(lru_idx, aligned_fault_vaddr);
  }
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

  auto v = HEAP_START + 0 * PAGE_SIZE;
  auto pte = mapper_t::get_protect(v);
  DEBUG("ALL %lu", pte);
  DEBUG("PTE_P %s", bool_to_str(pte_is_present(pte)));
  DEBUG("PTE_W %s", bool_to_str(pte_is_writable(pte)));
  DEBUG("PTE_A %s", bool_to_str(pte_is_accessed(pte)));
  DEBUG("PTE_D %s", bool_to_str(pte_is_dirty(pte)));
  set_permissions(v, pte & ~(uptr)(PTE_A | PTE_D));
  pte = mapper_t::get_protect(v);
  DEBUG("ALL %lu", pte);
  DEBUG("PTE_P %s", bool_to_str(pte_is_present(pte)));
  DEBUG("PTE_W %s", bool_to_str(pte_is_writable(pte)));
  DEBUG("PTE_A %s", bool_to_str(pte_is_accessed(pte)));
  DEBUG("PTE_D %s", bool_to_str(pte_is_dirty(pte)));
  trigger_write(0, 0xdead);

  // this successfully swaps the 2nd (1-index) page
  // even though the the 1st one is initially set with an older
  // fault time because it's accessed after a mark phase.
  // auto now = Clock::now();
  // pages[0].last_fault = now - std::chrono::milliseconds(1000);
  // pages[1].last_fault = now - std::chrono::milliseconds(600);
  // mark();
  // trigger_read(0);
  // print_pages();
  // trigger_read(60);
  // print_pages();

  printf("\n--- Exiting VM ---\n");
}

int main() {
  cache_area = aligned_alloc(PAGE_SIZE, CACHE_SIZE);
  swap_area = aligned_alloc(PAGE_SIZE, SWAP_SIZE);
  pages = new Page[NUM_PAGES];

  DEBUG("CACHE 0x%lx", (uptr)cache_area);
  DEBUG("SWAP 0x%lx", (uptr)swap_area);
  DEBUG("PAGES METADATA 0x%lx", (uptr)pages);

  constexpr s_volimem_config_t voli_config{
      .log_level = INFO,
      .host_page_type = VOLIMEM_NORMAL_PAGES,
      .guest_page_type = VOLIMEM_NORMAL_PAGES,
  };

  volimem_set_config(&voli_config);
  return volimem_start(nullptr, virtual_main);
}
