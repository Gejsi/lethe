#pragma once

#include <atomic>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

class AsyncLogger {
public:
  static AsyncLogger &instance() {
    static AsyncLogger instance;
    return instance;
  }

  void init(const std::string &filename);
  void log(const char *level, const char *fmt, ...);
  void stop();

private:
  AsyncLogger() = default;
  ~AsyncLogger();

  void worker_loop();

  std::ofstream file_;
  std::vector<std::string> buffer_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread worker_;
  std::atomic<bool> running_{false};
};
