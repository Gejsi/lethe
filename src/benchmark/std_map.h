#pragma once

#include <benchmark/data_interface.h>
#include <stdexcept>
#include <unordered_map>

class StdMap : public data_interface<uint64_t> {
public:
  StdMap() = default;

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
      return map_.at(key); // .at() throws an exception if the key doesn't exist
    } catch (const std::out_of_range &oor) {
      // The benchmark doesn't have a clear way to handle "not found",
      // so we'll return a sentinel value. 0 is a poor choice if it's a valid
      // value, but for this test it's acceptable.
      return 0;
    }
  }

  int is_null(uint64_t key) override {
    return map_.count(key) == 0; // .count() is 1 if key exists, 0 otherwise
  }

  uint64_t dummy() override { return map_.size(); };

private:
  std::unordered_map<uint64_t, uint64_t> map_;
};
