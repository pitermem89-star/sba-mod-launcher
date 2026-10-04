// SBA Shaders: changes the game's light, fog and ambient colour to give it a
// different "look": day (original), sunset, night, pastel.
//
// How it works (simple version):
//   1. We hook UnityEngine.Camera.get_main. The game calls it all the time, so
//      it is a free "clock" that ticks on the main thread.
//   2. Every ~30 ticks we read our settings (enabled / style / strength).
//   3. If something changed (or the game loaded a new scene and reset the
//      lighting), we set RenderSettings (ambient, fog) and the sun Light.
//
// Everything is in this single file. Uses only the Bearite API + il2cpp.
#include "bearite.hpp"

#include <dlfcn.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char* TAG = "sba-shaders";
constexpr const char* ASM = "UnityEngine.CoreModule";

struct Color { float r, g, b, a; };

// ---- il2cpp functions we need (found with dlsym) -------------------------
void* (*rt_invoke)(void*, void*, void**, void**) = nullptr;
void* (*obj_unbox)(void*) = nullptr;

// ---- Unity methods (MethodInfo*), filled in init() -----------------------
struct M {
  void *set_ambient, *get_ambient;
  void *set_fog, *get_fog;
  void *set_fog_color, *get_fog_color;
  void *set_fog_density, *get_fog_density;
  void *set_fog_mode, *get_fog_mode;
  void *get_sun;
  void *light_set_color, *light_get_color;
  void *light_set_intensity, *light_get_intensity;
} m;

bool g_ready = false;

void* invoke(void* method, void* self, void** args) {
  if (!method || !rt_invoke) return nullptr;
  void* none[1] = {nullptr};
  void* exc = nullptr;
  void* r = rt_invoke(method, self, args ? args : none, &exc);
  return exc ? nullptr : r;
}

Color get_color(void* method, void* self = nullptr) {
  Color c{0, 0, 0, 1};
  void* b = invoke(method, self, nullptr);
  if (b) c = *static_cast<Color*>(obj_unbox(b));
  return c;
}
float get_float(void* method, void* self = nullptr) {
  void* b = invoke(method, self, nullptr);
  return b ? *static_cast<float*>(obj_unbox(b)) : 0.f;
}
int get_int(void* method, void* self = nullptr) {
  void* b = invoke(method, self, nullptr);
  return b ? *static_cast<int*>(obj_unbox(b)) : 0;
}
bool get_bool(void* method, void* self = nullptr) {
  void* b = invoke(method, self, nullptr);
  return b ? *static_cast<bool*>(obj_unbox(b)) : false;
}
void set_color(void* method, Color c, void* self = nullptr) { void* a[1] = {&c}; invoke(method, self, a); }
void set_float(void* method, float v, void* self = nullptr) { void* a[1] = {&v}; invoke(method, self, a); }
void set_int(void* method, int v, void* self = nullptr) { void* a[1] = {&v}; invoke(method, self, a); }
void set_bool(void* method, bool v, void* self = nullptr) { void* a[1] = {&v}; invoke(method, self, a); }

Color mix(Color a, Color b, float t) {
  return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.f};
}
bool same(Color a, Color b) {
  return std::fabs(a.r - b.r) < 0.003f && std::fabs(a.g - b.g) < 0.003f && std::fabs(a.b - b.b) < 0.003f;
}

bool init() {
  if (g_ready) return true;
  void* h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
  if (!h) h = RTLD_DEFAULT;
  rt_invoke = reinterpret_cast<decltype(rt_invoke)>(dlsym(h, "il2cpp_runtime_invoke"));
  obj_unbox = reinterpret_cast<decltype(obj_unbox)>(dlsym(h, "il2cpp_object_unbox"));
  if (!rt_invoke || !obj_unbox) {
    bearite::log(BEARITE_LOG_ERROR, TAG, "il2cpp exports not found");
    return false;
  }
  auto rs = [](const char* name, int argc) { return bearite::find_method(ASM, "UnityEngine", "RenderSettings", name, argc); };
  auto lt = [](const char* name, int argc) { return bearite::find_method(ASM, "UnityEngine", "Light", name, argc); };
  m.set_ambient = rs("set_ambientLight", 1);
  m.get_ambient = rs("get_ambientLight", 0);
  m.set_fog = rs("set_fog", 1);
  m.get_fog = rs("get_fog", 0);
  m.set_fog_color = rs("set_fogColor", 1);
  m.get_fog_color = rs("get_fogColor", 0);
  m.set_fog_density = rs("set_fogDensity", 1);
  m.get_fog_density = rs("get_fogDensity", 0);
  m.set_fog_mode = rs("set_fogMode", 1);
  m.get_fog_mode = rs("get_fogMode", 0);
  m.get_sun = rs("get_sun", 0);
  m.light_set_color = lt("set_color", 1);
  m.light_get_color = lt("get_color", 0);
  m.light_set_intensity = lt("set_intensity", 1);
  m.light_get_intensity = lt("get_intensity", 0);
  if (!m.set_ambient || !m.get_ambient || !m.set_fog || !m.set_fog_color) {
    bearite::log(BEARITE_LOG_ERROR, TAG, "RenderSettings methods not found");
    return false;
  }
  g_ready = true;
  return true;
}

// ---- styles --------------------------------------------------------------
struct Preset {
  Color sun;        // sun light colour
  float sun_mult;   // sun brightness multiplier
  Color ambient;    // ambient (shadow) light colour
  Color fog;        // fog colour
  float density;    // fog thickness
};

bool get_preset(const std::string& style, Preset& p) {
  if (style == "sunset") { p = {{1.00f, 0.55f, 0.25f, 1}, 1.10f, {0.85f, 0.50f, 0.45f, 1}, {0.95f, 0.50f, 0.35f, 1}, 0.006f}; return true; }
  if (style == "night")  { p = {{0.35f, 0.45f, 0.90f, 1}, 0.25f, {0.12f, 0.16f, 0.35f, 1}, {0.04f, 0.07f, 0.18f, 1}, 0.012f}; return true; }
  if (style == "pastel") { p = {{1.00f, 0.95f, 0.95f, 1}, 1.00f, {1.00f, 0.85f, 0.90f, 1}, {0.85f, 0.90f, 1.00f, 1}, 0.004f}; return true; }
  return false;  // "day" = original
}

// The original values of the current scene (so "day" can restore them).
struct Saved {
  bool have = false;
  Color ambient{}, fog{}, sun{};
  bool fog_on = false;
  float density = 0, sun_i = 1;
  int mode = 1;
} saved;

Color g_last_ambient{-1, -1, -1, 1};  // the ambient colour we set last time
std::string g_last_key;
int g_tick = 0;

void* sun_light() { return invoke(m.get_sun, nullptr, nullptr); }

void capture_originals() {
  saved.ambient = get_color(m.get_ambient);
  saved.fog = get_color(m.get_fog_color);
  saved.fog_on = get_bool(m.get_fog);
  saved.density = get_float(m.get_fog_density);
  saved.mode = get_int(m.get_fog_mode);
  if (void* sun = sun_light()) {
    saved.sun = get_color(m.light_get_color, sun);
    saved.sun_i = get_float(m.light_get_intensity, sun);
  }
  saved.have = true;
}

void apply(const std::string& style, float t) {
  Preset p;
  bool is_style = get_preset(style, p);
  void* sun = sun_light();
  if (!is_style) {  // restore the original look
    set_color(m.set_ambient, saved.ambient);
    set_bool(m.set_fog, saved.fog_on);
    set_color(m.set_fog_color, saved.fog);
    set_float(m.set_fog_density, saved.density);
    set_int(m.set_fog_mode, saved.mode);
    if (sun) { set_color(m.light_set_color, saved.sun, sun); set_float(m.light_set_intensity, saved.sun_i, sun); }
    g_last_ambient = saved.ambient;
    return;
  }
  Color amb = mix(saved.ambient, p.ambient, t);
  set_color(m.set_ambient, amb);
  set_bool(m.set_fog, true);
  set_int(m.set_fog_mode, 3);  // FogMode.ExponentialSquared
  set_color(m.set_fog_color, mix(saved.fog, p.fog, t));
  set_float(m.set_fog_density, saved.density + (p.density - saved.density) * t);
  if (sun) {
    set_color(m.light_set_color, mix(saved.sun, p.sun, t), sun);
    set_float(m.light_set_intensity, saved.sun_i * (1.f + (p.sun_mult - 1.f) * t), sun);
  }
  g_last_ambient = amb;
}

void tick() {
  if (!init()) return;
  std::string en = bearite::setting("enabled", "true");
  bool enabled = (en == "true" || en == "1" || en == "True");
  std::string style = bearite::setting("style", "day");
  float t = static_cast<float>(atof(bearite::setting("strength", "1").c_str()));
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  if (!enabled) style = "day";

  // A new scene resets the lighting. Then the current ambient is not the one
  // we set, so these are the new originals.
  Color now = get_color(m.get_ambient);
  bool scene_changed = !saved.have || !same(now, g_last_ambient);
  if (scene_changed) {
    capture_originals();
    g_last_key.clear();
  }
  std::string key = style + "|" + std::to_string(t);
  if (key == g_last_key) return;
  g_last_key = key;
  apply(style, t);
  bearite::log(BEARITE_LOG_INFO, TAG, "style=%s strength=%.1f", style.c_str(), t);
}

// ---- the hook ------------------------------------------------------------
void* (*orig_get_main)(void*) = nullptr;

void* hk_get_main(void* method_info) {
  void* cam = orig_get_main ? orig_get_main(method_info) : nullptr;
  if (++g_tick >= 30) {
    g_tick = 0;
    tick();
  }
  return cam;
}

}  // namespace

// Entry point. IMPORTANT: copy the exact signature from your hello-mod / mod-menu.
extern "C" __attribute__((visibility("default"))) void bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  bool ok = bearite::hook(ASM, "UnityEngine", "Camera", "get_main", 0,
                          reinterpret_cast<void*>(&hk_get_main),
                          reinterpret_cast<void**>(&orig_get_main));
  bearite::log(ok ? BEARITE_LOG_INFO : BEARITE_LOG_ERROR, TAG, ok ? "loaded" : "hook failed");
}
