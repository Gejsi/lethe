#include <atomic>
#include <benchmark/data_interface.h>
#include <cstddef>
#include <unordered_map>

#include "../utils.h"

constexpr uptr HEAP_START = 0xffff800000000000;
constexpr usize HEAP_SIZE = 2 * GB;

inline std::atomic<uintptr_t> heap_current{HEAP_START};
inline const uintptr_t heap_end = HEAP_START + HEAP_SIZE;

template <typename T> class BumpAllocator {
public:
  // Required type definitions for STL allocator
  using value_type = T;
  using pointer = T *;
  using const_pointer = const T *;
  using reference = T &;
  using const_reference = const T &;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  // Rebind: allows allocator<T> to be converted to allocator<U>
  template <typename U> struct rebind {
    using other = BumpAllocator<U>;
  };

  BumpAllocator() noexcept = default;

  template <typename U> BumpAllocator(const BumpAllocator<U> &) noexcept {}

  T *allocate(size_t n) {
    size_t bytes = n * sizeof(T);
    uintptr_t old_current = heap_current.fetch_add(bytes);

    if (old_current + bytes > heap_end) {
      throw std::bad_alloc();
    }

    constexpr size_t alignment = std::max(alignof(T), size_t(8));
    uintptr_t aligned_current =
        (old_current + alignment - 1) & ~(alignment - 1);

    return reinterpret_cast<T *>(aligned_current);
  }

  void deallocate(T *ptr, size_t n) noexcept {
    // Bump allocator is a no-op for deallocation
    (void)ptr;
    (void)n;
  }

  // Maximum allocatable size
  size_type max_size() const noexcept { return (HEAP_SIZE / sizeof(T)); }
};

// Equality comparison (all instances are equal - stateless allocator)
template <typename T, typename U>
bool operator==(const BumpAllocator<T> &, const BumpAllocator<U> &) noexcept {
  return true;
}
template <typename T, typename U>
bool operator!=(const BumpAllocator<T> &, const BumpAllocator<U> &) noexcept {
  return false;
}

using BumpHeapMapAllocator = BumpAllocator<std::pair<const uint64_t, uint64_t>>;
using BumpHeapMap =
    std::unordered_map<uint64_t,                // Key
                       uint64_t,                // Value
                       std::hash<uint64_t>,     // Hasher
                       std::equal_to<uint64_t>, // Key equality checker
                       BumpHeapMapAllocator     // Allocator
                       >;

class BumpHeapMapDataLayer : public data_interface<uint64_t> {
public:
  BumpHeapMapDataLayer() = default;

  int insert(uint64_t key, uint64_t value) override {
    map_[key] = value;
    return 0;
  }

  int update(uint64_t key, uint64_t value) override {
    map_[key] = value;
    return 1;
  }

  uint64_t remove(uint64_t key) override { return map_.erase(key); }

  uint64_t get(uint64_t key) override {
    try {
      return map_.at(key);
    } catch (const std::out_of_range &oor) {
      return 0;
    }
  }

  int is_null(uint64_t key) override { return map_.count(key) == 0; }

  uint64_t dummy() override { return map_.size(); };

private:
  BumpHeapMap map_;
};

// ============================================================================
// USAGE EXAMPLES
// ============================================================================

/*
// Example 1: std::map with RDMA allocator
using RDMAMap = std::map<
  int,
  std::string,
  std::less<int>,
  BumpAllocator<std::pair<const int, std::string>>
>;

RDMAMap my_map;
my_map[42] = "hello";  // Allocated in RDMA heap

// Example 2: std::vector with RDMA allocator
using RDMAVector = std::vector<int, BumpAllocator<int>>;

RDMAVector vec;
vec.push_back(123);    // Allocated in RDMA heap

// Example 3: std::unordered_map (for key-value store benchmark)
using RDMAKVStore = std::unordered_map<
  std::string,
  std::string,
  std::hash<std::string>,
  std::equal_to<std::string>,
  BumpAllocator<std::pair<const std::string, std::string>>
>;

RDMAKVStore kv_store;
kv_store["key1"] = "value1";  // Both key and value in RDMA heap

// Example 4: Custom string with RDMA allocator
using RDMAString = std::basic_string<
  char,
  std::char_traits<char>,
  BumpAllocator<char>
>;

RDMAString str("Hello RDMA!");  // String data in RDMA heap!
*/
