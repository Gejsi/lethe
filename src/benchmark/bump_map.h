#include <atomic>
#include <benchmark/data_interface.h>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

// TODO: remove this hacky include
constexpr uintptr_t HEAP_START2 = 0xffff800000000000;
constexpr size_t HEAP_SIZE2 = (size_t)2 * 1024 * 1024 * 1024;

inline std::atomic<uintptr_t> heap_current{HEAP_START2};
inline const uintptr_t heap_end = HEAP_START2 + HEAP_SIZE2;

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
  size_type max_size() const noexcept { return (HEAP_SIZE2 / sizeof(T)); }
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

class BumpMapDataLayer : public data_interface<uint64_t> {
public:
  BumpMapDataLayer() = default;

  int insert(uint64_t key, uint64_t value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[key] = value;
    return 0;
  }

  int update(uint64_t key, uint64_t value) override {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[key] = value;
    return 1;
  }

  uint64_t remove(uint64_t key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.erase(key);
  }

  uint64_t get(uint64_t key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
      return map_.at(key);
    } catch (const std::out_of_range &oor) {
      return 0;
    }
  }

  int is_null(uint64_t key) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.count(key) == 0;
  }

  uint64_t dummy() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
  };

private:
  std::unordered_map<
      uint64_t,                // Key
      uint64_t,                // Value
      std::hash<uint64_t>,     // Hasher
      std::equal_to<uint64_t>, // Key equality checker
      BumpAllocator<std::pair<const uint64_t, uint64_t>>> // Allocator
      map_;

  std::mutex mutex_;
};
