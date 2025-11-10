#pragma once

#include <cstddef>
#include <pthread.h>
#include <stdio.h>
#include <sys/cdefs.h>

#define MAX_SMALL_LOG 25

struct free_chunk_t {
  size_t nbb;
  size_t padding;
  struct free_chunk_t *next;
};

class heap_content_t {
private:
  void *static_chunk;
  struct free_chunk_t *free_list;
  struct free_chunk_t *arenas[MAX_SMALL_LOG];

public:
  static heap_content_t *init_heap_content(void *m);
  int format_heap_content(size_t nbb);
  void *halloc(size_t n);
  bool hstatic(void *addr, size_t n);
  void hfree(void *ptr);
  size_t hsize(void *ptr);
  void list();
};
