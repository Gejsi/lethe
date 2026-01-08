#include <array>
#include <iomanip>
#include <thread>

#include "utils.h"

constexpr const char *bool_to_str(bool b) noexcept {
  return b ? "true" : "false";
}

void sleep_ms(u64 ms) { std::this_thread::sleep_for(Milliseconds(ms)); }

std::string human_readable_bytes(u64 bytes) {
  constexpr std::array suffixes{"B", "KB", "MB", "GB", "TB", "PB", "EB"};
  usize i = 0;
  double count = static_cast<double>(bytes);

  while (count >= 1024.0 && i < suffixes.size() - 1) {
    count /= 1024.0;
    i++;
  }

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << count << " " << suffixes[i];
  return oss.str();
}
