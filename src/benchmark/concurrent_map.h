#include <memory>
#include <unordered_map>

// A dummy mutex that performs no locking operation.
struct NoLockMutex {
  void lock() noexcept {}
  void unlock() noexcept {}
  bool try_lock() noexcept { return true; }
};

/**
 * @tparam Key The key type.
 * @tparam Value The value type.
 * @tparam ThreadSafe If true, a std::mutex is used to protect access.
 *                    If false, a dummy mutex is used
 * @tparam Allocator The allocator to use for the underlying map
 */
template <typename Key, typename Value, bool ThreadSafe = false,
          typename Allocator = std::allocator<std::pair<const Key, Value>>>
class ConcurrentUnorderedMap {
private:
  using MapType = std::unordered_map<Key, Value, std::hash<Key>,
                                     std::equal_to<Key>, Allocator>;

  // Use a real mutex if ThreadSafe is true, otherwise use the dummy one.
  using MutexType =
      typename std::conditional_t<ThreadSafe, std::mutex, NoLockMutex>;

  MapType map_;
  mutable MutexType mutex_; // 'mutable' allows locking in const methods

public:
  explicit ConcurrentUnorderedMap(const Allocator &alloc = Allocator())
      : map_(alloc) {}

  void insert(const Key &key, const Value &value) {
    std::lock_guard<MutexType> lock(mutex_);
    map_[key] = value;
  }

  void update(const Key &key, const Value &value) {
    std::lock_guard<MutexType> lock(mutex_);
    map_[key] = value;
  }

  size_t erase(const Key &key) {
    std::lock_guard<MutexType> lock(mutex_);
    return map_.erase(key);
  }

  Value get(const Key &key) const {
    std::lock_guard<MutexType> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
      return it->second;
    }
    // Return a default-constructed value if not found.
    // The benchmark expects this for a failed 'get'.
    return Value{};
  }

  size_t count(const Key &key) const {
    std::lock_guard<MutexType> lock(mutex_);
    return map_.count(key);
  }

  size_t size() const {
    std::lock_guard<MutexType> lock(mutex_);
    return map_.size();
  }
};
