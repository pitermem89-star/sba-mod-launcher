// Stable C interface between the loader and mods.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEARITE_ABI_VERSION 2
#define BEARITE_EXPORT __attribute__((visibility("default")))

// Same numbers as Android log priorities.
enum { BEARITE_LOG_DEBUG = 3, BEARITE_LOG_INFO = 4, BEARITE_LOG_WARN = 5, BEARITE_LOG_ERROR = 6 };

typedef struct BeariteApi BeariteApi;

struct BeariteApi {
  // ---- ABI v1 (never changes) ----
  uint32_t abi_version;
  uint32_t struct_size;
  const char* game_version;  // e.g. "13.0.5", empty if unknown
  const char* mod_dir;       // folder of this mod (contains mod.json)
  void (*log)(int level, const char* tag, const char* msg);

  // ---- ABI v2 ----
  void* internal;  // loader private, do not touch

  // Hooks an IL2CPP method by name. argc = number of arguments WITHOUT `this`.
  // Installed as soon as the game's IL2CPP runtime is ready; until then
  // *out_original stays NULL. Returns 1 if accepted, 0 on bad arguments.
  int (*hook_method)(const BeariteApi* api, const char* assembly, const char* ns,
                     const char* klass, const char* method, int argc,
                     void* replacement, void** out_original);

  // Native address of an IL2CPP method, NULL if runtime not ready or not found.
  void* (*find_method)(const BeariteApi* api, const char* assembly, const char* ns,
                       const char* klass, const char* method, int argc);

  // 1 when the IL2CPP runtime is ready.
  int (*il2cpp_ready)(const BeariteApi* api);

  // Per-mod settings (<mod_dir>/settings.json). Copies the value into `out`.
  // Returns 1 if the key existed, 0 if the default was used.
  int (*get_setting)(const BeariteApi* api, const char* key, const char* default_value,
                     char* out, uint32_t out_size);
  int (*set_setting)(const BeariteApi* api, const char* key, const char* value);
};

// True if the loader's API struct is new enough to contain `field`.
#define BEARITE_API_HAS(api, field) \
  ((api) && (api)->struct_size >= offsetof(BeariteApi, field) + sizeof((api)->field))

// Entry points a mod can export (only on_load is required).
typedef int (*BeariteOnLoadFn)(const BeariteApi* api);  // 0 = ok
typedef void (*BeariteOnUpdateFn)(float dt);            // every frame
typedef void (*BeariteOnUnloadFn)(void);                // loader disabled the mod

#ifdef __cplusplus
}
#endif
