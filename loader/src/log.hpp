// Logging to logcat and to a file. Header-only.
#pragma once

#include <android/log.h>

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace bearite {

class Logger {
 public:
  static Logger& get() {
    static Logger instance;
    return instance;
  }

  // Opens (and truncates) the log file.
  void open_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_) fclose(file_);
    file_ = fopen(path.c_str(), "w");
  }

  void write(int prio, const char* tag, const char* msg) {
    __android_log_print(prio, "Bearite", "[%s] %s", tag, msg);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!file_) return;
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    char stamp[16];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
    static const char kLetters[] = "??VDIWEF";
    char letter = (prio >= 0 && prio < 8) ? kLetters[prio] : '?';
    fprintf(file_, "%s %c [%s] %s\n", stamp, letter, tag, msg);
    fflush(file_);
  }

 private:
  Logger() = default;
  std::mutex mutex_;
  FILE* file_ = nullptr;
};

__attribute__((format(printf, 3, 4)))
inline void logf(int prio, const char* tag, const char* fmt, ...) {
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Logger::get().write(prio, tag, buf);
}

}  // namespace bearite

#define BLOG_I(tag, ...) ::bearite::logf(ANDROID_LOG_INFO, tag, __VA_ARGS__)
#define BLOG_W(tag, ...) ::bearite::logf(ANDROID_LOG_WARN, tag, __VA_ARGS__)
#define BLOG_E(tag, ...) ::bearite::logf(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
