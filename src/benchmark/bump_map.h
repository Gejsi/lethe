#include <atomic>
#include <benchmark/data_interface.h>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

class BumpArena {
public:
  BumpArena(uintptr_t start, size_t size)
      : heap_end_(start + size), heap_current_(start) {}

  uintptr_t allocate(size_t bytes, size_t alignment) {
    uintptr_t old_current = heap_current_.fetch_add(bytes);
    uintptr_t aligned_current =
        (old_current + alignment - 1) & ~(alignment - 1);

    if (aligned_current + bytes > heap_end_) {
      // Return 0 as an indicator of failure instead of throwing,
      // since allocators shouldn't throw from allocate.
      // The allocator will handle turning this into an exception.
      return 0;
    }
    return aligned_current;
  }

private:
  const uintptr_t heap_end_;
  std::atomic<uintptr_t> heap_current_;
};

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

  BumpArena *arena_;

  explicit BumpAllocator(BumpArena *arena) noexcept : arena_(arena) {}

  template <typename U>
  BumpAllocator(const BumpAllocator<U> &other) noexcept
      : arena_(other.arena_) {}

  T *allocate(size_t n) {
    if (!arena_) {
      throw std::bad_alloc();
    }

    constexpr size_t alignment = alignof(T);
    size_t bytes = n * sizeof(T);

    uintptr_t ptr = arena_->allocate(bytes, alignment);
    if (ptr == 0) {
      throw std::bad_alloc();
    }
    return reinterpret_cast<T *>(ptr);
  }

  void deallocate(T *ptr, size_t n) noexcept {
    // Bump allocator is a no-op for deallocation
    (void)ptr;
    (void)n;
  }
};

// Equality comparison (all instances are equal - stateless allocator)
template <typename T, typename U>
bool operator==(const BumpAllocator<T> &a, const BumpAllocator<U> &b) noexcept {
  return a.arena_ == b.arena_;
}
template <typename T, typename U>
bool operator!=(const BumpAllocator<T> &a, const BumpAllocator<U> &b) noexcept {
  return a.arena_ != b.arena_;
}

class BumpMapDataLayer : public data_interface<uint64_t> {
public:
  BumpMapDataLayer(uintptr_t start, size_t size)
      : arena_(start, size),
        map_(BumpAllocator<std::pair<const uint64_t, uint64_t>>(&arena_)) {}

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
  BumpArena arena_;

  std::unordered_map<
      uint64_t,                // Key
      uint64_t,                // Value
      std::hash<uint64_t>,     // Hasher
      std::equal_to<uint64_t>, // Key equality checker
      BumpAllocator<std::pair<const uint64_t, uint64_t>>> // Allocator
      map_;

  std::mutex mutex_;
};
