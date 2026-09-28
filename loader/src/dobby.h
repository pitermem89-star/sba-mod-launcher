#pragma once
extern "C" void* A64HookFunction(void* symbol, void* replace, void** result);
inline int DobbyHook(void* target, void* replacement, void** orig) {
  A64HookFunction(target, replacement, orig);
  return 0;
}
