#pragma once

#define USE_ASYNC_LOGGER

#include "types.h"

// Interface for a generic block-based storage backend.
class Storage {
public:
  virtual ~Storage() = default;

  // Reads a page-sized block from a given offset
  // in the backend into a local memory buffer.
  virtual int read_page(void *local_dest, u64 remote_offset) = 0;

  // Writes a page-sized block from a local memory buffer
  // to a given offset in the backend.
  virtual int write_page(void *local_src, u64 remote_offset) = 0;

  // Returns the base virtual address of the local cache buffer
  // that this storage backend is configured to use for I/O.
  virtual void *get_cache_base_addr() const = 0;
};
