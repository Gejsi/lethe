#pragma once

#include "swapper.h"

bool pte_is_present(uptr pte);
bool pte_is_writable(uptr pte);
bool pte_is_accessed(uptr pte);
bool pte_is_dirty(uptr pte);
