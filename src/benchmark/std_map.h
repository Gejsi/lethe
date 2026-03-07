#pragma once

#include <benchmark/data_interface.h>

#include "concurrent_map.h"
#include "types.h"

class StdMap : public data_interface<u64> {
public:
  StdMap() = default;

  int insert(u64 key, u64 value) override {
    map_.insert(key, value);
    return 0;
  }

  int update(u64 key, u64 value) override {
    map_.update(key, value);
    return 1;
  }

  u64 remove(u64 key) override { return map_.erase(key); }

  u64 get(u64 key) override { return map_.get(key); }

  int is_null(u64 key) override {
    return map_.count(key) == 0; // .count() is 1 if key exists, 0 otherwise
  }

  u64 dummy() override { return map_.size(); };

private:
  ConcurrentUnorderedMap<u64, u64, false> map_;
};
