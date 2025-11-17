#include <benchmark/data_interface.h>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <unordered_map>

#define MAX_SMALL_LOG 25
// 256 could be better, we have to evaluate that minimal unit of allocation
#define CHSIZE 64
// size of the static region
#define STATIC_SIZE 0x80000

// n is supposed to be a power of 2
static size_t round_n(size_t size, size_t n) { return ((size - 1) & -n) + n; }

struct FreeChunk {
  size_t nbb;
  size_t padding;
  struct FreeChunk *next;
};

class ChunkAllocator {
private:
  void *static_chunk;
  struct FreeChunk *free_list;
  struct FreeChunk *arenas[MAX_SMALL_LOG];

public:
  static ChunkAllocator *init_heap_content(void *m) {
    return (ChunkAllocator *)m;
  }

  int format_heap_content(size_t nbb) {
    size_t static_offset = round_n(sizeof(*this), 1 << 12);
    size_t first_node_offset = static_offset + round_n(STATIC_SIZE, CHSIZE);

    this->free_list = (struct FreeChunk *)((char *)this + first_node_offset);
    this->free_list->nbb = nbb - first_node_offset;
    this->free_list->next = NULL;
    this->static_chunk = (struct FreeChunk *)((char *)this + static_offset);
    memset(this->arenas, 0, sizeof(struct FreeChunk *) * MAX_SMALL_LOG);
    // printf("static chunk %p\n", this->static_chunk);
    return 0;
  }

  void *halloc(size_t n) {
    void *object = NULL;
    n = round_n(n + 2 * sizeof(size_t), CHSIZE);
    // we allocate by chunks of CHSIZE, must be at least sizeof(free_chunk_t)
    // bigger chunk than requested, bcs of metadata and this line return

    int idx = 64 - __builtin_clzll(n - 1); // arena id
    if (idx < MAX_SMALL_LOG)
      n = 1 << idx;

    if (idx < MAX_SMALL_LOG && arenas[idx]) {
      object = &arenas[idx]->next;
      arenas[idx] = arenas[idx]->next;
      // printf("%p %ld\n", object, psize(object));
      return object;
    }

    for (struct FreeChunk **cur = &free_list; *cur; cur = &(*cur)->next) {
      struct FreeChunk *res = *cur;
      if (res->nbb == n) {
        // consumes the whole chunk
        *cur = res->next;
        object =
            &res->next; // first location we can write (avoiding res->nbb field)
        break;
      } else if (n < res->nbb) {
        // splits the chunk
        struct FreeChunk *new_node = (struct FreeChunk *)((char *)res + n);
        new_node->nbb = res->nbb - n;
        new_node->next = res->next;
        res->nbb = n;
        *cur = new_node;
        object = &res->next;
        break;
      }
    }
    // printf("%p %ld\n", object, psize(object));
    return object;
  }

  void hfree(void *ptr) {
    struct FreeChunk *node =
        (struct FreeChunk *)((char *)ptr - 2 * sizeof(size_t));
    int idx = 64 - __builtin_clzll(node->nbb - 1);
    if (idx < MAX_SMALL_LOG) {
      node->next = arenas[idx];
      arenas[idx] = node;
    } else {
      node->next = this->free_list;
      this->free_list = node;
    }
    // printf("Pfree %ld %p\n", node->nbb, ptr);
  }

  size_t hsize(void *ptr) {
    struct FreeChunk *node =
        (struct FreeChunk *)((char *)ptr - 2 * sizeof(size_t));
    return node->nbb - sizeof(size_t);
  }

  // checks if chosen address range belongs to static region
  bool hstatic(void *addr, size_t n) {
    return addr >= this->static_chunk &&
           ((char *)addr + n) <= ((char *)this->static_chunk + STATIC_SIZE);
  }

  void list() {
    printf(".................... %p\n", (void *)free_list);
    for (struct FreeChunk **cur = &free_list; *cur; cur = &(*cur)->next)
      printf("free chunk %ld\n", (*cur)->nbb);
  }
};

inline ChunkAllocator *g_heap_allocator = nullptr;

template <typename T> struct LinkedBumpAllocator {
  using value_type = T;

  LinkedBumpAllocator() {
    constexpr uintptr_t HEAP_START = 0xffff800000000000;
    constexpr size_t HEAP_SIZE = (size_t)2 * 1024 * 1024 * 1024;

    if (!g_heap_allocator) {
      printf("Initializing allocator on hardcoded HEAP_START...\n");
      g_heap_allocator = ChunkAllocator::init_heap_content((void *)HEAP_START);
      g_heap_allocator->format_heap_content(HEAP_SIZE);
    }
  }

  template <typename U>
  constexpr LinkedBumpAllocator(const LinkedBumpAllocator<U> &) noexcept {}

  T *allocate(std::size_t n) {
    if (n > std::size_t(-1) / sizeof(T)) {
      throw std::bad_alloc();
    }

    if (!g_heap_allocator) {
      throw std::runtime_error("Allocator was not initialized!");
    }

    void *p = g_heap_allocator->halloc(n * sizeof(T));
    if (!p) {
      throw std::bad_alloc();
    }
    return static_cast<T *>(p);
  }

  void deallocate(T *p, std::size_t n) noexcept {
    (void)n;
    if (g_heap_allocator) {
      g_heap_allocator->hfree(p);
    }
  }
};

template <typename T, typename U>
bool operator==(const LinkedBumpAllocator<T> &,
                const LinkedBumpAllocator<U> &) {
  return true;
}

template <typename T, typename U>
bool operator!=(const LinkedBumpAllocator<T> &,
                const LinkedBumpAllocator<U> &) {
  return false;
}

class LinkedBumpMapDataLayer : public data_interface<uint64_t> {
public:
  LinkedBumpMapDataLayer() = default;

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
  std::unordered_map<uint64_t,                // Key
                     uint64_t,                // Value
                     std::hash<uint64_t>,     // Hasher
                     std::equal_to<uint64_t>, // Key equality checker
                     LinkedBumpAllocator<std::pair<const uint64_t, uint64_t>>>
      map_;
};
