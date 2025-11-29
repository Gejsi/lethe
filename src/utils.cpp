#include <thread>

#include "utils.h"

constexpr const char *bool_to_str(bool b) noexcept {
  return b ? "true" : "false";
}

void sleep_ms(u64 ms) { std::this_thread::sleep_for(Milliseconds(ms)); }
