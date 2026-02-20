#pragma once

#include <atomic>
#include <benchmark/data_interface.h>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>

#include "types.h"

class BumpArena {
public:
  BumpArena(uptr start, usize size)
      : heap_end_(start + size), heap_current_(start) {}

  uptr allocate(usize bytes, usize alignment) {
    uptr current = heap_current_.load(std::memory_order_relaxed);
    uptr aligned;
    uptr new_current;

    do {
      aligned = (current + alignment - 1) & ~(alignment - 1);
      new_current = aligned + bytes;

      // OOM
      if (new_current > heap_end_) {
        return 0;
      }
    } while (!heap_current_.compare_exchange_weak(current, new_current,
                                                  std::memory_order_relaxed,
                                                  std::memory_order_relaxed));

    return aligned;
  }

private:
  const uptr heap_end_;
  std::atomic<uptr> heap_current_;
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

  T *allocate(usize n) {
    if (!arena_) {
      throw std::bad_alloc();
    }

    constexpr usize alignment = alignof(T);
    usize bytes = n * sizeof(T);

    uptr ptr = arena_->allocate(bytes, alignment);
    if (ptr == 0) {
      throw std::bad_alloc();
    }
    return reinterpret_cast<T *>(ptr);
  }

  void deallocate(T *, usize) noexcept {
    // Bump allocator is a no-op for deallocation
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

class BumpMapDataLayer : public data_interface<u64> {
public:
  BumpMapDataLayer(uptr start, usize size)
      : arena_(start, size),
        map_(BumpAllocator<std::pair<const u64, u64>>(&arena_)) {}

  int insert(u64 key, u64 value) override {
    // std::lock_guard<std::mutex> lock(mutex_);
    map_[key] = value;
    return 0;
  }

  int update(u64 key, u64 value) override {
    // std::lock_guard<std::mutex> lock(mutex_);
    map_[key] = value;
    return 1;
  }

  u64 remove(u64 key) override {
    // std::lock_guard<std::mutex> lock(mutex_);
    return map_.erase(key);
  }

  u64 get(u64 key) override {
    // std::lock_guard<std::mutex> lock(mutex_);
    try {
      return map_.at(key);
    } catch (const std::out_of_range &oor) {
      return 0;
    }
  }

  int is_null(u64 key) override {
    // std::lock_guard<std::mutex> lock(mutex_);
    return map_.count(key) == 0;
  }

  u64 dummy() override {
    // std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
  };

private:
  BumpArena arena_;

  std::unordered_map<u64,                // Key
                     u64,                // Value
                     std::hash<u64>,     // Hasher
                     std::equal_to<u64>, // Key equality checker
                     BumpAllocator<std::pair<const u64, u64>>> // Allocator
      map_;

  // std::mutex mutex_;
};
