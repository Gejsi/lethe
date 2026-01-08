#pragma once

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <unordered_map>

#include "types.h"

// Thread-safe static arena implemented with a simple bump allocator
class Arena {
  u8 *storage_; // Owns the memory
  uptr start_;
  uptr end_;
  std::atomic<uptr> current_;

public:
  explicit Arena(usize size)
      : storage_(nullptr), start_(0), end_(0), current_(0) {
    storage_ = static_cast<u8 *>(operator new(
        size, std::align_val_t{alignof(std::max_align_t)}));

    start_ = reinterpret_cast<uptr>(storage_);
    end_ = start_ + size;
    current_.store(start_, std::memory_order_relaxed);
  }

  ~Arena() {
    operator delete(storage_, std::align_val_t{alignof(std::max_align_t)});
  }

  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;

  uptr allocate(usize bytes, usize alignment) {
    assert((alignment & (alignment - 1)) == 0);

    uptr current = current_.load(std::memory_order_relaxed);
    uptr aligned;
    uptr new_current;

    do {
      aligned = (current + alignment - 1) & ~(alignment - 1);
      new_current = aligned + bytes;

      // OOM
      if (new_current > end_) {
        return 0;
      }
    } while (!current_.compare_exchange_weak(current, new_current,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed));

    return aligned;
  }

  void deallocate(void *, size_t) {
    // Bump allocator is a no-op for deallocation
  }
};

// The STL wrapper for the Arena
template <typename T> struct ArenaAllocator {
  using value_type = T;
  Arena *arena_;

  explicit ArenaAllocator(Arena *a) : arena_(a) {}

  template <typename U>
  ArenaAllocator(const ArenaAllocator<U> &other) : arena_(other.arena_) {}

  T *allocate(size_t n) {
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

  void deallocate(T *p, size_t n) { arena_->deallocate(p, n * sizeof(T)); }

  friend bool operator==(const ArenaAllocator &a, const ArenaAllocator &b) {
    return a.arena_ == b.arena_;
  }
  friend bool operator!=(const ArenaAllocator &a, const ArenaAllocator &b) {
    return a.arena_ != b.arena_;
  }
};

template <typename T> using ArenaList = std::list<T, ArenaAllocator<T>>;

template <typename Key, typename T, typename Hash = std::hash<Key>,
          typename Eq = std::equal_to<Key>>
using ArenaUnorderedMap =
    std::unordered_map<Key, T, Hash, Eq,
                       ArenaAllocator<std::pair<const Key, T>>>;
