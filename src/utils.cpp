#include "utils.h"
#include "volimem/x86constants.h"

bool pte_is_present(uptr pte) { return pte & PTE_P; }
bool pte_is_writable(uptr pte) { return pte & PTE_W; }
bool pte_is_accessed(uptr pte) { return pte & PTE_A; }
bool pte_is_dirty(uptr pte) { return pte & PTE_D; }
