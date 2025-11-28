#include <cstdarg>
#include <cstdio>
#include <iostream>

#include "logger.h"

void AsyncLogger::init(const std::string &filename) {
  file_.open(filename, std::ios::out | std::ios::trunc);
  running_ = true;
  worker_ = std::thread(&AsyncLogger::worker_loop, this);
}

void AsyncLogger::log(const char *level, const char *fmt, ...) {
  if (!running_) {
    return;
  }

  char buffer[2048];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  std::string message = "[" + std::string(level) + "] " + std::string(buffer);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    buffer_.push_back(std::move(message));
  }
  cv_.notify_one();
}

void AsyncLogger::worker_loop() {
  std::vector<std::string> write_queue;

  while (running_) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return !buffer_.empty() || !running_; });

      // Swap buffers to release lock immediately
      std::swap(write_queue, buffer_);
    }

    // Write to disk without blocking the main threads
    for (const auto &msg : write_queue) {
      if (file_.is_open())
        file_ << msg << std::endl;
      else
        std::cout << msg << std::endl; // Fallback
    }

    if (file_.is_open()) {
      // Ensure data hits disk
      file_.flush();
    }

    write_queue.clear();
  }
}

void AsyncLogger::stop() {
  running_ = false;
  cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
  if (file_.is_open())
    file_.close();
}

AsyncLogger::~AsyncLogger() { stop(); }
