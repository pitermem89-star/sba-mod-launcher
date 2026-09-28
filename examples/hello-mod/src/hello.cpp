// Example mod: logs a greeting, hooks Application.version, counts time in on_update.
#include "bearite.hpp"

static void* (*orig_get_version)(const void* method) = nullptr;
static int g_version_calls = 0;
static double g_elapsed = 0;
static double g_next_report = 5;

// Replacement for the static getter UnityEngine.Application.version.
static void* hook_get_version(const void* method) {
  if (g_version_calls++ == 0)
    bearite::log(BEARITE_LOG_INFO, "hello", "the game asked for Application.version");
  return orig_get_version ? orig_get_version(method) : nullptr;
}

extern "C" BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  std::string greeting = bearite::setting("greeting", "Hello from Bearite!");
  bearite::set_setting("greeting", greeting.c_str());  // creates settings.json on first run
  bearite::log(BEARITE_LOG_INFO, "hello", "%s (game %s)", greeting.c_str(), api->game_version);

  if (!bearite::hook("UnityEngine.CoreModule", "UnityEngine", "Application", "get_version", 0,
                     reinterpret_cast<void*>(hook_get_version),
                     reinterpret_cast<void**>(&orig_get_version)))
    bearite::log(BEARITE_LOG_WARN, "hello", "hook was not accepted");
  return 0;
}

extern "C" BEARITE_EXPORT void bearite_on_update(float dt) {
  g_elapsed += dt;
  if (g_elapsed >= g_next_report) {
    bearite::log(BEARITE_LOG_INFO, "hello", "alive for %.0f s", g_elapsed);
    g_next_report += 30;
  }
}

extern "C" BEARITE_EXPORT void bearite_on_unload(void) {
  bearite::log(BEARITE_LOG_INFO, "hello", "bye");
}
