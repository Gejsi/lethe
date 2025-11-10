#include <benchmark/data_interface.h>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <unordered_map>

#include "heap_allocator.h"

inline heap_content_t *g_lethe_heap_ctx = nullptr;

inline void initialize_lethe_allocator() {
  constexpr uintptr_t HEAP_START = 0xffff800000000000;
  constexpr size_t HEAP_SIZE = 2ULL * 1024 * 1024 * 1024;

  if (!g_lethe_heap_ctx) {
    printf("Initializing LetheAllocator on hardcoded HEAP_START...\n");
    g_lethe_heap_ctx = heap_content_t::init_heap_content((void *)HEAP_START);
    g_lethe_heap_ctx->format_heap_content(HEAP_SIZE);
  }
}

template <typename T> struct LetheAllocator {
  using value_type = T;

  LetheAllocator() { initialize_lethe_allocator(); }

  template <typename U>
  constexpr LetheAllocator(const LetheAllocator<U> &) noexcept {}

  T *allocate(std::size_t n) {
    if (n > std::size_t(-1) / sizeof(T)) {
      throw std::bad_alloc();
    }

    if (!g_lethe_heap_ctx) {
      throw std::runtime_error("LetheAllocator context was not initialized!");
    }

    void *p = g_lethe_heap_ctx->halloc(n * sizeof(T));
    if (!p) {
      throw std::bad_alloc();
    }
    return static_cast<T *>(p);
  }

  void deallocate(T *p, std::size_t n) noexcept {
    (void)n;
    if (g_lethe_heap_ctx) {
      g_lethe_heap_ctx->hfree(p);
    }
  }
};

template <typename T, typename U>
bool operator==(const LetheAllocator<T> &, const LetheAllocator<U> &) {
  return true;
}

template <typename T, typename U>
bool operator!=(const LetheAllocator<T> &, const LetheAllocator<U> &) {
  return false;
}

// 1. Define the specific allocator type for the map's internal nodes.
//    The map allocates std::pair<const Key, Value>.
using LetheMapAllocator = LetheAllocator<std::pair<const uint64_t, uint64_t>>;

// 2. Define the map type using the custom allocator.
using LetheMapType =
    std::unordered_map<uint64_t,                // Key
                       uint64_t,                // Value
                       std::hash<uint64_t>,     // Hasher
                       std::equal_to<uint64_t>, // Key equality checker
                       LetheMapAllocator        // YOUR CUSTOM ALLOCATOR
                       >;

// 3. Create the data_interface implementation using this new map type.
class LetheMapInterface : public data_interface<uint64_t> {
public:
  LetheMapInterface() = default;

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
