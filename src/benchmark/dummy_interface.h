#pragma once

#include <benchmark/data_interface.h>

class dummy_interface : public data_interface<uint64_t> {
public:
  virtual int insert(uint64_t key, uint64_t value) {
    (void)key;
    (void)value;
    // printf("Insert key %ld value %ld\n", key, value);
    return 0;
  }
  virtual int update(uint64_t key, uint64_t value) {
    (void)key;
    (void)value;
    // printf("Update key %ld value %ld\n", key, value);
    return 1;
  }
  virtual uint64_t remove(uint64_t key) {
    (void)key;
    // printf("Remove key %ld\n", key);
    return 1;
  }
  virtual uint64_t get(uint64_t key) {
    (void)key;
    // printf("Get key %ld\n", key);
    return 1;
  }
  virtual int is_null(uint64_t key) {
    (void)key;
    return 0;
  }
  virtual uint64_t dummy() { return 0; };
};
