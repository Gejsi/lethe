#include <atomic>
#include <benchmark/data_interface.h>
#include <cstddef>
#include <limits>
#include <unordered_map>

#include "../utils.h"

constexpr usize HEAP_SIZE = 1 * GB;
constexpr uptr HEAP_START = 0xffff800000000000;
inline std::atomic<uintptr_t> heap_current{HEAP_START};
inline const uintptr_t heap_end = HEAP_START + HEAP_SIZE;

template <typename T> class HeapAllocator {
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
    using other = HeapAllocator<U>;
  };

  // Constructors
  HeapAllocator() noexcept = default;

  template <typename U> HeapAllocator(const HeapAllocator<U> &) noexcept {}

  /*
  // Allocate n elements of type T
  T *allocate(std::size_t n) {
    if (n == 0)
      return nullptr;

    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }

    size_t bytes = n * sizeof(T);

    // Determine alignment requirement
    constexpr size_t alignment = std::max(alignof(T), size_t(8));

    static std::atomic<uptr> heap_current{HEAP_START};
    static const uptr heap_end = HEAP_START + HEAP_SIZE;

    uptr old_current, new_current, aligned_current;

    do {
      old_current = heap_current.load(std::memory_order_relaxed);

      // Align the current pointer UP to the required alignment
      aligned_current = (old_current + alignment - 1) & ~(alignment - 1);

      // Check if we have space
      if (aligned_current + bytes > heap_end) {
        throw std::bad_alloc();
      }

      // New current is aligned address + bytes
      new_current = aligned_current + bytes;

    } while (!heap_current.compare_exchange_weak(old_current, new_current,
                                                 std::memory_order_relaxed));

    // Return the properly aligned address
    return reinterpret_cast<T *>(aligned_current);
  }

  // Deallocate (no-op for bump allocator)
  void deallocate(T *ptr, std::size_t n) noexcept {
    // Bump allocator doesn't support deallocation
    // Memory is reclaimed when swapper evicts pages
    (void)ptr;
    (void)n;
  }
  */

  T *allocate(std::size_t n) {
    size_t bytes_to_alloc = n * sizeof(T);
    uintptr_t old_current = heap_current.fetch_add(bytes_to_alloc);

    if (old_current + bytes_to_alloc > heap_end) {
      throw std::bad_alloc();
    }

    // Your alignment fix from before was correct and should be here.
    constexpr size_t alignment = alignof(T);
    uintptr_t aligned_current =
        (old_current + alignment - 1) & ~(alignment - 1);

    // This is a more complex but correct way for a multi-threaded bump
    // allocator For now, the simple fetch_add reveals the core issue. Let's
    // stick with the simple one to see the fix, but the aligned version is
    // better.

    return reinterpret_cast<T *>(
        aligned_current); // Let's stick to the unaligned
                          // version for now to see the fix
  }

  void deallocate(T *ptr, std::size_t n) noexcept {
    // Bump allocator is a no-op for deallocation
    (void)ptr;
    (void)n;
  }

  // Maximum allocatable size
  size_type max_size() const noexcept { return (HEAP_SIZE / sizeof(T)); }
};

// Equality comparison (all instances are equal - stateless allocator)
template <typename T, typename U>
bool operator==(const HeapAllocator<T> &, const HeapAllocator<U> &) noexcept {
  return true;
}
template <typename T, typename U>
bool operator!=(const HeapAllocator<T> &, const HeapAllocator<U> &) noexcept {
  return false;
}

using HeapMapAllocator = HeapAllocator<std::pair<const uint64_t, uint64_t>>;
using LetheMapType =
    std::unordered_map<uint64_t,                // Key
                       uint64_t,                // Value
                       std::hash<uint64_t>,     // Hasher
                       std::equal_to<uint64_t>, // Key equality checker
                       HeapMapAllocator         // Allocator
                       >;

class HeapMapDataLayer : public data_interface<uint64_t> {
public:
  HeapMapDataLayer() = default;

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
  LetheMapType map_;
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
  RDMAAllocator<std::pair<const int, std::string>>
>;

RDMAMap my_map;
my_map[42] = "hello";  // Allocated in RDMA heap!

// Example 2: std::vector with RDMA allocator
using RDMAVector = std::vector<int, RDMAAllocator<int>>;

RDMAVector vec;
vec.push_back(123);    // Allocated in RDMA heap!

// Example 3: std::unordered_map (for key-value store benchmark)
using RDMAKVStore = std::unordered_map<
  std::string,
  std::string,
  std::hash<std::string>,
  std::equal_to<std::string>,
  RDMAAllocator<std::pair<const std::string, std::string>>
>;

RDMAKVStore kv_store;
kv_store["key1"] = "value1";  // Both key and value in RDMA heap!

// Example 4: Custom string with RDMA allocator
using RDMAString = std::basic_string<
  char,
  std::char_traits<char>,
  RDMAAllocator<char>
>;

RDMAString str("Hello RDMA!");  // String data in RDMA heap!
*/
