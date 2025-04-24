#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <sys/mman.h>

#include "swapper.h"
#include "tests/rr.cpp"
#include "volimem/mapper.h"
#include "volimem/volimem.h"

Page *pages;
void *cache_area; // client
void *swap_area;  // server
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

//             // Mark as inaccessible - USING UNMAP for consistency with
//             swap_out
//             // If VoliMem had protect(PROT_NONE), it might be better.
//             mapper_t::unmap(target_vaddr, PAGE_SIZE);
//             mapper_t::flush(target_vaddr, PAGE_SIZE); // Essential after
//             unmap/permission change

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

void print_pages() {
  for (auto &page : std::span(pages, NUM_PAGES)) {
    page.print();
  }
}

usize find_victim_rr() {
  static usize next_idx = 0;
  usize idx = next_idx;
  next_idx = (next_idx + 1) % NUM_PAGES;
  DEBUG("Filling cache slot %zu", idx);
  return idx;
}

int find_victim_probed(uptr target_vaddr) {
  for (usize i = 0; i < NUM_PAGES; i++) {
    if (pages[i].vaddr == target_vaddr && pages[i].state == PageState::Probed) {
      return (int)i;
    }
  }
  return -1;
}

void set_permissions(uptr vaddr, uptr flags) {
  mapper_t::mprotect(vaddr, PAGE_SIZE, flags);
  mapper_t::flush(vaddr, PAGE_SIZE);
}

void mark() {
  auto now = std::chrono::steady_clock::now();
  usize new_probed_count = 0;
  usize old_probed_count = 0;
  usize free_count = 0;

  for (auto &page : std::span(pages, NUM_PAGES)) {
    switch (page.state) {
    case PageState::Mapped:
      // update protection level to cause a fault in case of read/write
      set_permissions(page.vaddr, PTE_NONE);
      page.last_scan = now;
      page.state = PageState::Probed;
      new_probed_count++;
      break;
    case PageState::Probed:
      old_probed_count++;
      break;
    case PageState::Free:
      free_count++;
      break;
    }
  }

  DEBUG("Marked pages: newly probed=%zu, already probed=%zu, free=%zu",
        new_probed_count, old_probed_count, free_count);
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
  // assert(victim_page.state != PageState::Free);
  if (victim_page.state != PageState::Free) {
    auto victim_vaddr = victim_page.vaddr;
    INFO("Swapping OUT victim: gva = 0x%lx, gpa = 0x%lx, state = %s",
         victim_vaddr, mapper_t::gva_to_gpa((void *)victim_vaddr),
         page_state_to_str(victim_page.state));

    auto victim_offset = victim_vaddr - HEAP_START;
    auto swap_dst = (uptr)swap_area + victim_offset;

    swap_out(victim_vaddr, swap_dst);

    victim_page.vaddr = 0;
    victim_page.state = PageState::Free;
    victim_page.last_fault = {};
    victim_page.last_scan = {};
  }
}

void swap_in_page(usize target_idx, uptr fault_addr) {
  auto aligned_fault_vaddr = fault_addr & ~(PAGE_SIZE - 1);
  auto cache_offset = target_idx * PAGE_SIZE;
  auto cache_slot = (uptr)cache_area + cache_offset;
  auto cache_gpa = mapper_t::gva_to_gpa((void *)cache_slot);
  INFO("Swapping IN: 0x%lx (aligned: 0x%lx), cache_gpa 0x%lx", fault_addr,
       aligned_fault_vaddr, cache_gpa);

  auto fault_offset = aligned_fault_vaddr - HEAP_START;
  auto swap_src = (uptr)swap_area + fault_offset;

  // copy data from swap to cache slot
  // swap_in(aligned_fault_vaddr, swap_src, cache_gpa);
  mapper_t::map_gpt(aligned_fault_vaddr, cache_gpa, PAGE_SIZE, PTE_P | PTE_W);
  memcpy((void *)cache_slot, (void *)swap_src, PAGE_SIZE);

  auto &target_page = pages[target_idx];
  target_page.vaddr = aligned_fault_vaddr;
  target_page.state = PageState::Mapped;
  target_page.last_fault = std::chrono::steady_clock::now();
  target_page.last_scan = {};
}

void handle_fault(void *fault_addr) {
  DEBUG("Inside fault handler: %p", fault_addr);

  auto aligned_fault_vaddr = (uptr)fault_addr & ~(PAGE_SIZE - 1);
  // check if this is a fault on a page we probed
  auto probed_idx = find_victim_probed(aligned_fault_vaddr);

  if (probed_idx != -1) {
    DEBUG("Restoring access permissions for 0x%lx", aligned_fault_vaddr);
    set_permissions(aligned_fault_vaddr, PTE_P | PTE_W | PTE_U);

    pages[probed_idx].state = PageState::Mapped;
    pages[probed_idx].last_fault = std::chrono::steady_clock::now();
  } else {
    auto victim_idx = find_victim_rr();
    swap_out_page(victim_idx);
    swap_in_page(victim_idx, (uptr)fault_addr);
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
  //   auto gpa = allocate_page();
  //   printf("Page[%zu] gva=0x%lx, gpa = 0x%lx\n", i, vaddr, gpa);
  //   mapper_t::map_gpt(vaddr, gpa, PAGE_SIZE, PTE_P | PTE_W);
  // }

  printf("[Test 1] Write to Page 50 (Should FAULT IN)\n");
  {
    auto page = (uptr *)(HEAP_START + PAGE_SIZE * 50);
    printf("Attempting access to %p (state: %s) (voli-mapped: %s)\n",
           (void *)page, page_state_to_str(pages[0].state),
           bool_to_str(mapper_t::is_mapped(page)));
    *page = 0xDEADBEEF;
    printf("Write succeeded: 0x%lx\n", *page);
  }

  printf("\n[Test 1.5] Manually marking\n");
  mark();

  printf("\n[Test 2] Write to Page 60 (Should FAULT IN)\n");
  {
    auto page = (uptr *)(HEAP_START + PAGE_SIZE * 60);
    printf("Attempting access to %p (state: %s) (voli-mapped: %s)\n",
           (void *)page, page_state_to_str(pages[0].state),
           bool_to_str(mapper_t::is_mapped(page)));
    *page = 0xCAFEBABE;
    printf("Write succeeded: 0x%lx\n", *page);
  }

  // printf("\n[Test 3] Read from Page 50 (Should SUCCEED)\n");
  // {
  //   auto page = (uptr *)(HEAP_START + PAGE_SIZE * 50);
  //   printf("Attempting access to %p (state: %s) (voli-mapped: %s)\n",
  //          (void *)page, page_state_to_str(pages[0].state),
  //          bool_to_str(mapper_t::is_mapped(page)));
  //   printf("Read succeeded: 0x%lx\n", *page);
  // }

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
