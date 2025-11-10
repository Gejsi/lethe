#include <cstddef>
#include <string.h>

#include "heap_allocator.h"

// 256 could be better, we have to evaluate that minimal unit of allocation
#define CHSIZE 64
// size of the static region
#define STATIC_SIZE 0x80000

// n is supposed to be a power of 2
static size_t round_n(size_t size, size_t n) { return ((size - 1) & -n) + n; }

heap_content_t *heap_content_t::init_heap_content(void *m) {
  return (heap_content_t *)m;
}

int heap_content_t::format_heap_content(size_t nbb) {
  size_t static_offset = round_n(sizeof(*this), 1 << 12);
  size_t first_node_offset = static_offset + round_n(STATIC_SIZE, CHSIZE);

  this->free_list = (struct free_chunk_t *)((char *)this + first_node_offset);
  this->free_list->nbb = nbb - first_node_offset;
  this->free_list->next = NULL;
  this->static_chunk = (struct free_chunk_t *)((char *)this + static_offset);
  memset(this->arenas, 0, sizeof(struct free_chunk_t *) * MAX_SMALL_LOG);
  // printf("static chunk %p\n", this->static_chunk);
  return 0;
}

void heap_content_t::list() {
  printf(".................... %p\n", (void *)free_list);
  for (struct free_chunk_t **cur = &free_list; *cur; cur = &(*cur)->next)
    printf("free chunk %ld\n", (*cur)->nbb);
}

// checks if chosen address range belongs to static region
bool heap_content_t::hstatic(void *addr, size_t n) {
  return addr >= this->static_chunk &&
         ((char *)addr + n) <= ((char *)this->static_chunk + STATIC_SIZE);
}

void *heap_content_t::halloc(size_t n) { // buddy allocator

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

  for (struct free_chunk_t **cur = &free_list; *cur; cur = &(*cur)->next) {
    struct free_chunk_t *res = *cur;
    if (res->nbb == n) {
      // consumes the whole chunk
      *cur = res->next;
      object =
          &res->next; // first location we can write (avoiding res->nbb field)
      break;
    } else if (n < res->nbb) {
      // splits the chunk
      struct free_chunk_t *new_node = (struct free_chunk_t *)((char *)res + n);
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

void heap_content_t::hfree(void *ptr) {
  struct free_chunk_t *node =
      (struct free_chunk_t *)((char *)ptr - 2 * sizeof(size_t));
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

size_t heap_content_t::hsize(void *ptr) {
  struct free_chunk_t *node =
      (struct free_chunk_t *)((char *)ptr - 2 * sizeof(size_t));
  return node->nbb - sizeof(size_t);
}
