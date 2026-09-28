// Stable C interface between the loader and mods. Extended in stage 3.
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEARITE_ABI_VERSION 1

typedef struct BeariteApi {
  uint32_t abi_version;
  uint32_t struct_size;
  const char* game_version;  // e.g. "13.0.5", may be empty if unknown
  const char* mod_dir;       // folder of this mod (contains mod.json)
  // level: Android log priority (3 debug, 4 info, 5 warn, 6 error)
  void (*log)(int level, const char* tag, const char* msg);
} BeariteApi;

// Every mod must export this. Return 0 on success, anything else = failure.
typedef int (*BeariteOnLoadFn)(const BeariteApi* api);

#ifdef __cplusplus
}
#endif
