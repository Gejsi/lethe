#pragma once

#include <benchmark/data_interface.h>

#include "concurrent_map.h"

class StdMap : public data_interface<uint64_t> {
public:
  StdMap() = default;

  int insert(uint64_t key, uint64_t value) override {
    map_.insert(key, value);
    return 0;
  }

  int update(uint64_t key, uint64_t value) override {
    map_.update(key, value);
    return 1;
  }

  uint64_t remove(uint64_t key) override { return map_.erase(key); }

  uint64_t get(uint64_t key) override { return map_.get(key); }

  int is_null(uint64_t key) override {
    return map_.count(key) == 0; // .count() is 1 if key exists, 0 otherwise
  }

  uint64_t dummy() override { return map_.size(); };

private:
  ConcurrentUnorderedMap<uint64_t, uint64_t, false> map_;
};
