#pragma once

#include <cstddef>
#include <pthread.h>
#include <stdio.h>
#include <sys/cdefs.h>

#define MAX_SMALL_LOG 25

struct FreeChunk {
  size_t nbb;
  size_t padding;
  struct FreeChunk *next;
};

class BuddyAllocator {
private:
  void *static_chunk;
  struct FreeChunk *free_list;
  struct FreeChunk *arenas[MAX_SMALL_LOG];

public:
  static BuddyAllocator *init_heap_content(void *m);
  int format_heap_content(size_t nbb);
  void *halloc(size_t n);
  bool hstatic(void *addr, size_t n);
  void hfree(void *ptr);
  size_t hsize(void *ptr);
  void list();
};
