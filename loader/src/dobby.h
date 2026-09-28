// Thin Dobby-compatible shim over And64InlineHook (arm64 only).
#pragma once

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <map>

extern "C" void* A64HookFunction(void* symbol, void* replace, void** result);

namespace dobby_shim {

constexpr size_t kPatchSize = 16;  // max bytes And64 overwrites

inline std::map<void*, unsigned char*>& saved() {
  static std::map<void*, unsigned char*> m;
  return m;
}

inline bool write_code(void* target, const void* data) {
  const uintptr_t page = static_cast<uintptr_t>(getpagesize());
  uintptr_t start = reinterpret_cast<uintptr_t>(target) & ~(page - 1);
  uintptr_t end = (reinterpret_cast<uintptr_t>(target) + kPatchSize + page - 1) & ~(page - 1);
  if (mprotect(reinterpret_cast<void*>(start), end - start,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    return false;
  memcpy(target, data, kPatchSize);
  __builtin___clear_cache(static_cast<char*>(target),
                          static_cast<char*>(target) + kPatchSize);
  mprotect(reinterpret_cast<void*>(start), end - start, PROT_READ | PROT_EXEC);
  return true;
}

}  // namespace dobby_shim

inline int DobbyHook(void* target, void* replacement, void** orig) {
  if (!target || !replacement) return -1;
  auto& s = dobby_shim::saved();
  if (s.count(target)) return -1;  // already hooked
  auto* backup = new unsigned char[dobby_shim::kPatchSize];
  memcpy(backup, target, dobby_shim::kPatchSize);
  A64HookFunction(target, replacement, orig);
  s[target] = backup;
  return 0;
}

inline int DobbyDestroy(void* target) {
  auto& s = dobby_shim::saved();
  auto it = s.find(target);
  if (it == s.end()) return -1;
  bool ok = dobby_shim::write_code(target, it->second);
  delete[] it->second;
  s.erase(it);
  return ok ? 0 : -1;
}
