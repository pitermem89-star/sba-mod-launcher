// C++ helpers for mod authors. Include this file in your mod.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

#include "../loader/src/abi.h"

namespace bearite {

inline const BeariteApi*& api() {
  static const BeariteApi* a = nullptr;
  return a;
}

// Call once at the start of bearite_on_load.
inline void init(const BeariteApi* a) { api() = a; }

__attribute__((format(printf, 3, 4)))
inline void log(int level, const char* tag, const char* fmt, ...) {
  if (!api() || !api()->log) return;
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  api()->log(level, tag, buf);
}

// Hooks an IL2CPP method by name. argc = arguments without `this`.
// Your replacement receives `this` first (instance methods) and MethodInfo* last.
inline bool hook(const char* assembly, const char* ns, const char* klass, const char* method,
                 int argc, void* replacement, void** original) {
  if (!BEARITE_API_HAS(api(), hook_method)) return false;
  return api()->hook_method(api(), assembly, ns, klass, method, argc, replacement, original) != 0;
}

inline void* find_method(const char* assembly, const char* ns, const char* klass,
                         const char* method, int argc) {
  if (!BEARITE_API_HAS(api(), find_method)) return nullptr;
  return api()->find_method(api(), assembly, ns, klass, method, argc);
}

inline std::string setting(const char* key, const char* fallback = "") {
  if (!BEARITE_API_HAS(api(), get_setting)) return fallback;
  char buf[512];
  api()->get_setting(api(), key, fallback, buf, sizeof(buf));
  return buf;
}

inline bool set_setting(const char* key, const char* value) {
  if (!BEARITE_API_HAS(api(), set_setting)) return false;
  return api()->set_setting(api(), key, value) != 0;
}

}  // namespace bearite
