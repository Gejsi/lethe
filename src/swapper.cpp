#include <volimem/mapper.h>

#include "swapper.h"
#include "utils.h"

void set_permissions(uptr vaddr, u64 flags, bool flush) {
  mapper_t::mpermit(vaddr, PAGE_SIZE, flags);
  if (flush) {
    mapper_t::flush(vaddr, PAGE_SIZE);
  }
}

void clear_permissions(uptr vaddr, u64 flags) {
  auto perms = mapper_t::get_protect(vaddr);
  mapper_t::mpermit(vaddr, PAGE_SIZE, perms & ~flags);
  // if (flush) {
  //   mapper_t::flush(vaddr, PAGE_SIZE);
  // }
}

bool pte_is_present(u64 pte) { return pte & PTE_P; }
bool pte_is_writable(u64 pte) { return pte & PTE_W; }
bool pte_is_accessed(u64 pte) { return pte & PTE_A; }
bool pte_is_dirty(u64 pte) { return pte & PTE_D; }

void map(uptr gva, uptr gpa) {
  mapper_t::map_gpt(gva, gpa, PAGE_SIZE, PTE_P | PTE_W);
}

void unmap(uptr gva) {
  // unmap the page from the guest page table
  mapper_t::unmap(gva, PAGE_SIZE);
  // ensure that unmap actually invalidates the TLB entry
  mapper_t::flush(gva, PAGE_SIZE);
}
