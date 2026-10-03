// visuals.cpp: "Visual Styles" mod for Super Bear Adventure (v0.2).
//
// The game's shaders ignore Unity lighting (sun / ambient), so changing
// lights only recolours the sky. This version tints the colour of EVERY
// material in the game (world, trees, houses, characters, sky) and, if
// the game allows it, adds fog. Style 0 puts everything back exactly as it was.
//
// Every optional step can fail on its own without stopping the mod.
// Everything it finds (or does not find) is written to bearite.log.
//
// Exports for the Bearite loader: bearite_on_load, bearite_on_update, bearite_on_unload.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

#include "../../api/bearite.hpp"

__attribute__((format(printf, 2, 3)))
static void say(int level, const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (bearite::api()) bearite::log(level, "visuals", "%s", buf);
  else fprintf(stderr, "%s\n", buf);
}

static std::string read_file(const std::string& path) {
  std::string out;
  if (FILE* f = fopen(path.c_str(), "rb")) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
  }
  return out;
}

// ============================================================ 1. il2cpp
// Exported il2cpp functions, taken straight from libil2cpp.so.

namespace il {

struct Api {
  void* (*domain_get)();
  void** (*domain_get_assemblies)(void*, size_t*);
  void* (*assembly_get_image)(void*);
  const char* (*image_get_name)(void*);
  void* (*class_from_name)(void*, const char*, const char*);
  void* (*class_get_methods)(void*, void**);
  void* (*class_get_parent)(void*);
  const char* (*method_get_name)(void*);
  uint32_t (*method_get_param_count)(void*);
  void* (*method_get_param)(void*, uint32_t);
  char* (*type_get_name)(void*);
  void* (*runtime_invoke)(void*, void*, void**, void**);
  void* (*string_new)(const char*);
  void* (*class_get_type)(void*);
  void* (*type_get_object)(void*);
  void* (*object_unbox)(void*);
  uint32_t (*gchandle_new)(void*, int);
  void (*free_mem)(void*);
  const uint16_t* (*string_chars)(void*);
  int32_t (*string_length)(void*);
  bool ok = false;
};
static Api A;
static void* g_lib = nullptr;

template <class T> static T sym(const char* name) {
  void* p = g_lib ? dlsym(g_lib, name) : nullptr;
  if (!p) p = dlsym(RTLD_DEFAULT, name);
  return reinterpret_cast<T>(p);
}

static bool init() {
  if (A.ok) return true;
  g_lib = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
#define L(field, name) A.field = sym<decltype(A.field)>(name)
  L(domain_get, "il2cpp_domain_get");
  L(domain_get_assemblies, "il2cpp_domain_get_assemblies");
  L(assembly_get_image, "il2cpp_assembly_get_image");
  L(image_get_name, "il2cpp_image_get_name");
  L(class_from_name, "il2cpp_class_from_name");
  L(class_get_methods, "il2cpp_class_get_methods");
  L(class_get_parent, "il2cpp_class_get_parent");
  L(method_get_name, "il2cpp_method_get_name");
  L(method_get_param_count, "il2cpp_method_get_param_count");
  L(method_get_param, "il2cpp_method_get_param");
  L(type_get_name, "il2cpp_type_get_name");
  L(runtime_invoke, "il2cpp_runtime_invoke");
  L(string_new, "il2cpp_string_new");
  L(class_get_type, "il2cpp_class_get_type");
  L(type_get_object, "il2cpp_type_get_object");
  L(object_unbox, "il2cpp_object_unbox");
  L(gchandle_new, "il2cpp_gchandle_new");
  L(free_mem, "il2cpp_free");
  L(string_chars, "il2cpp_string_chars");
  L(string_length, "il2cpp_string_length");
#undef L
  A.ok = A.domain_get && A.domain_get_assemblies && A.assembly_get_image && A.image_get_name &&
         A.class_from_name && A.class_get_methods && A.class_get_parent && A.method_get_name &&
         A.method_get_param_count && A.method_get_param && A.type_get_name && A.runtime_invoke &&
         A.string_new && A.class_get_type && A.type_get_object && A.object_unbox &&
         A.gchandle_new && A.string_chars && A.string_length;
  if (!A.ok) say(BEARITE_LOG_ERROR, "il2cpp API is incomplete (libil2cpp.so not loaded yet?)");
  return A.ok;
}

// "UnityEngine.CoreModule.dll" matches "UnityEngine.CoreModule".
static bool name_match(const char* have, const char* want) {
  size_t hl = strlen(have);
  if (hl > 4 && strcmp(have + hl - 4, ".dll") == 0) hl -= 4;
  return strlen(want) == hl && strncmp(have, want, hl) == 0;
}

static void* find_class(const char* asm_name, const char* ns, const char* name) {
  size_t cnt = 0;
  void** list = A.domain_get_assemblies(A.domain_get(), &cnt);
  if (!list) return nullptr;
  for (size_t i = 0; i < cnt; ++i) {
    void* img = A.assembly_get_image(list[i]);
    const char* nm = img ? A.image_get_name(img) : nullptr;
    if (nm && name_match(nm, asm_name)) return A.class_from_name(img, ns, name);
  }
  return nullptr;
}

// Method by name and argument count, looking through base classes too.
// p0 (optional) = type name of the first parameter, e.g. "System.String".
static void* find_method(void* klass, const char* name, int argc, const char* p0 = nullptr) {
  if (!klass) return nullptr;
  for (void* k = klass; k; k = A.class_get_parent(k)) {
    void* it = nullptr;
    while (void* m = A.class_get_methods(k, &it)) {
      if (strcmp(A.method_get_name(m), name) != 0) continue;
      if (static_cast<int>(A.method_get_param_count(m)) != argc) continue;
      if (p0 && argc > 0) {
        char* tn = A.type_get_name(A.method_get_param(m, 0));
        bool same = tn && strcmp(tn, p0) == 0;
        if (tn && A.free_mem) A.free_mem(tn);
        if (!same) continue;
      }
      return m;
    }
  }
  return nullptr;
}

static bool g_exc = false;  // true if the last call() threw a managed exception

static void* call(void* method, void* obj, std::initializer_list<void*> args = {}) {
  g_exc = false;
  if (!method) return nullptr;
  void* buf[8];
  size_t n = 0;
  for (void* p : args) if (n < 8) buf[n++] = p;
  void* exc = nullptr;
  void* r = A.runtime_invoke(method, obj, n ? buf : nullptr, &exc);
  g_exc = exc != nullptr;
  return g_exc ? nullptr : r;
}

static void* keep(void* obj) {  // keep a managed object alive for the GC
  if (obj) A.gchandle_new(obj, 0);
  return obj;
}

static void* type_object(void* klass) { return keep(A.type_get_object(A.class_get_type(klass))); }

static uint8_t* arr_data(void* arr) { return static_cast<uint8_t*>(arr) + 0x20; }
static size_t arr_len(void* arr) {
  return arr ? *reinterpret_cast<size_t*>(static_cast<uint8_t*>(arr) + 0x18) : 0;
}

static std::string str_utf8(void* s) {  // managed string -> ASCII
  std::string out;
  if (!s || !A.string_chars || !A.string_length) return out;
  const uint16_t* c = A.string_chars(s);
  int n = A.string_length(s);
  for (int i = 0; i < n && i < 120; ++i) out += c[i] < 128 ? static_cast<char>(c[i]) : '?';
  return out;
}

}  // namespace il

// ============================================================ 2. styles

struct Col { float r = 0, g = 0, b = 0, a = 1; };

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

struct Style {
  const char* name;
  float world[3];   // colour multiplier for world, characters, objects
  float sky[3];     // colour multiplier for the sky material
  float fog_col[3]; // fog colour
  float fog_den;    // fog density (0 = do not touch fog)
};

// Index = value of the "style" setting. 0 = the game as it was.
static const Style kStyles[4] = {
    {"day (original)", {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 0.0f},
    {"sunset", {1.12f, 0.76f, 0.56f}, {1.25f, 0.70f, 0.60f}, {1.0f, 0.55f, 0.32f}, 0.004f},
    {"night", {0.30f, 0.40f, 0.80f}, {0.20f, 0.26f, 0.65f}, {0.04f, 0.06f, 0.18f}, 0.010f},
    {"pastel", {1.10f, 1.08f, 1.18f}, {1.10f, 1.10f, 1.30f}, {0.85f, 0.90f, 1.0f}, 0.005f},
};

// ============================================================ 3. mod

namespace vis {

using namespace il;

struct Config {
  bool enabled = true;
  int style = 0;
  float strength = 1;
  bool operator==(const Config& o) const {
    return enabled == o.enabled && style == o.style && std::fabs(strength - o.strength) < 0.001f;
  }
};

static std::string g_dir;
static Config g_cfg;
static bool g_ready = false, g_dead = false, g_touched = false;
static int g_tries = 0;
static float g_t_retry = 10, g_t_cfg = 10, g_t_scan = 10;

// classes and methods
static void *k_mat, *t_mat;
static void *m_has, *m_get_col, *m_set_col, *m_get_shader, *m_get_exp_dummy;
static void *o_get_name, *find_all;
static void *rs_get_fog, *rs_set_fog, *rs_get_fmode, *rs_set_fmode, *rs_get_fcol, *rs_set_fcol,
    *rs_get_fden, *rs_set_fden;
static bool g_fog_ok = false;

// property names, created once as managed strings
static const char* kProps[] = {"_Color", "_BaseColor", "_TintColor", "_Tint", "_SkyTint", "_GroundColor"};
static const int kPropCount = sizeof(kProps) / sizeof(kProps[0]);
static void* g_prop[kPropCount];
static void* g_exposure_str = nullptr;

struct Entry {
  void* mat;
  int prop;
  Col orig;
  bool sky;
};
static std::vector<Entry> g_entries;
static std::map<void*, int> g_known;
static std::map<std::string, int> g_shader_count;
static int g_skipped = 0;

struct FogOrig { bool valid = false; bool fog = false; int mode = 0; Col col; float den = 0; };
static FogOrig g_fog;

// ---- typed calls
static Col get_col(void* m, void* obj, void* arg) {
  Col c;
  if (void* r = call(m, obj, {arg})) memcpy(&c, A.object_unbox(r), sizeof(Col));
  return c;
}
static float get_f(void* m, void* obj) {
  void* r = call(m, obj);
  return r ? *static_cast<float*>(A.object_unbox(r)) : 0.0f;
}
static int get_i(void* m, void* obj) {
  void* r = call(m, obj);
  return r ? *static_cast<int*>(A.object_unbox(r)) : 0;
}
static bool get_b(void* m, void* obj, std::initializer_list<void*> a = {}) {
  void* r = call(m, obj, a);
  return r && *static_cast<uint8_t*>(A.object_unbox(r)) != 0;
}
static void set_f(void* m, float f) { call(m, nullptr, {&f}); }
static void set_i(void* m, int i) { call(m, nullptr, {&i}); }
static void set_b(void* m, bool b) { uint8_t v = b ? 1 : 0; call(m, nullptr, {&v}); }
static void set_c(void* m, Col c) { call(m, nullptr, {&c}); }

static std::string obj_name(void* o) { return o ? str_utf8(call(o_get_name, o)) : std::string(); }

// ---- settings.json in the mod folder first (always fresh), then the loader
static bool file_value(const std::string& text, const char* key, double& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t p = text.find(pat);
  if (p == std::string::npos) return false;
  p = text.find(':', p + pat.size());
  if (p == std::string::npos) return false;
  ++p;
  while (p < text.size() && (text[p] == ' ' || text[p] == '\n' || text[p] == '\r' || text[p] == '\t')) ++p;
  if (text.compare(p, 4, "true") == 0) { out = 1; return true; }
  if (text.compare(p, 5, "false") == 0) { out = 0; return true; }
  if (p < text.size() && text[p] == '"') ++p;
  char* end = nullptr;
  double v = strtod(text.c_str() + p, &end);
  if (end == text.c_str() + p) return false;
  out = v;
  return true;
}

static bool get_setting_num(const std::string& file, const char* key, double& out) {
  if (file_value(file, key, out)) return true;
  std::string s = bearite::setting(key, "");
  if (s.empty()) return false;
  if (s == "true") { out = 1; return true; }
  if (s == "false") { out = 0; return true; }
  out = strtod(s.c_str(), nullptr);
  return true;
}

static Config read_config() {
  Config c;
  std::string file = read_file(g_dir + "/settings.json");
  double v;
  if (get_setting_num(file, "enabled", v)) c.enabled = v != 0;
  if (get_setting_num(file, "style", v)) c.style = static_cast<int>(v);
  if (get_setting_num(file, "strength", v)) c.strength = static_cast<float>(v);
  c.style = std::min(3, std::max(0, c.style));
  c.strength = std::min(1.0f, std::max(0.0f, c.strength));
  return c;
}

// ---- setup
static bool setup() {
  if (!il::init()) return false;
  const char* core = "UnityEngine.CoreModule";
  k_mat = find_class(core, "UnityEngine", "Material");
  void* k_obj = find_class(core, "UnityEngine", "Object");
  void* k_res = find_class(core, "UnityEngine", "Resources");
  void* k_rs = find_class(core, "UnityEngine", "RenderSettings");
  if (!k_mat || !k_obj) {
    say(BEARITE_LOG_WARN, "setup: Material/Object class not found yet (try %d)", g_tries);
    return false;
  }
  t_mat = type_object(k_mat);

  m_has = find_method(k_mat, "HasProperty", 1, "System.String");
  m_get_col = find_method(k_mat, "GetColor", 1, "System.String");
  m_set_col = find_method(k_mat, "SetColor", 2, "System.String");
  m_get_shader = find_method(k_mat, "get_shader", 0);
  o_get_name = find_method(k_obj, "get_name", 0);

  find_all = find_method(k_res, "FindObjectsOfTypeAll", 1, "System.Type");
  const char* find_src = "Resources.FindObjectsOfTypeAll";
  if (!find_all) { find_all = find_method(k_obj, "FindObjectsOfType", 1, "System.Type"); find_src = "Object.FindObjectsOfType"; }

  say(BEARITE_LOG_INFO, "methods: HasProperty=%d GetColor=%d SetColor=%d get_shader=%d get_name=%d find=%s(%d)",
      m_has != nullptr, m_get_col != nullptr, m_set_col != nullptr, m_get_shader != nullptr,
      o_get_name != nullptr, find_src, find_all != nullptr);

  if (!m_has || !m_get_col || !m_set_col || !o_get_name || !find_all) {
    say(BEARITE_LOG_ERROR, "setup failed: a required Material method was not found");
    return false;
  }

  for (int i = 0; i < kPropCount; ++i) g_prop[i] = keep(A.string_new(kProps[i]));
  g_exposure_str = keep(A.string_new("_Exposure"));

  // fog is optional
  rs_get_fog = find_method(k_rs, "get_fog", 0);       rs_set_fog = find_method(k_rs, "set_fog", 1);
  rs_get_fmode = find_method(k_rs, "get_fogMode", 0); rs_set_fmode = find_method(k_rs, "set_fogMode", 1);
  rs_get_fcol = find_method(k_rs, "get_fogColor", 0); rs_set_fcol = find_method(k_rs, "set_fogColor", 1);
  rs_get_fden = find_method(k_rs, "get_fogDensity", 0); rs_set_fden = find_method(k_rs, "set_fogDensity", 1);
  g_fog_ok = rs_get_fog && rs_set_fog && rs_get_fmode && rs_set_fmode && rs_get_fcol && rs_set_fcol &&
             rs_get_fden && rs_set_fden;
  if (g_fog_ok) {
    g_fog.fog = get_b(rs_get_fog, nullptr);
    g_fog.mode = get_i(rs_get_fmode, nullptr);
    g_fog.col = get_col(rs_get_fcol, nullptr, nullptr);
    g_fog.den = get_f(rs_get_fden, nullptr);
    g_fog.valid = true;
    say(BEARITE_LOG_INFO, "fog: on=%d mode=%d density=%.4f", g_fog.fog, g_fog.mode, g_fog.den);
  } else {
    say(BEARITE_LOG_WARN, "fog: RenderSettings fog methods not found, fog is skipped");
  }

  say(BEARITE_LOG_INFO, "setup done");
  g_ready = true;
  return true;
}

// ---- finding materials
static bool skip_shader(const std::string& s) {
  static const char* bad[] = {"UI/", "UI-", "TextMeshPro", "TMP", "Sprites/", "Hidden/", "GUI", "Text"};
  for (const char* b : bad) if (s.find(b) != std::string::npos) return true;
  return false;
}

static void examine(void* mat) {
  std::string sname = m_get_shader ? obj_name(call(m_get_shader, mat)) : std::string("?");
  if (g_exc) return;
  if (skip_shader(sname)) { ++g_skipped; return; }
  bool sky = sname.find("Skybox") != std::string::npos ||
             (g_exposure_str && get_b(m_has, mat, {g_exposure_str}));
  bool any = false;
  for (int i = 0; i < kPropCount; ++i) {
    if (!get_b(m_has, mat, {g_prop[i]})) continue;
    Col c = get_col(m_get_col, mat, g_prop[i]);
    if (g_exc) continue;
    g_entries.push_back({mat, i, c, sky});
    any = true;
  }
  if (any) ++g_shader_count[sname + (sky ? " [sky]" : "")];
}

static bool scan() {
  void* arr = call(find_all, nullptr, {t_mat});
  if (g_exc || !arr) return false;
  size_t n = arr_len(arr);
  void** items = reinterpret_cast<void**>(arr_data(arr));
  bool added = false;
  size_t before = g_entries.size();
  for (size_t i = 0; i < n; ++i) {
    void* m = items[i];
    if (!m || g_known.count(m)) continue;
    keep(m);
    g_known[m] = 1;
    examine(m);
  }
  added = g_entries.size() != before;
  if (added) say(BEARITE_LOG_INFO, "materials: %zu found, %zu colours tracked, %d skipped (UI/text)", g_known.size(), g_entries.size(), g_skipped);
  return added;
}

static void log_shaders() {
  int shown = 0;
  for (auto& kv : g_shader_count) {
    if (++shown > 14) break;
    say(BEARITE_LOG_INFO, "shader: %s x%d", kv.first.c_str(), kv.second);
  }
}

// ---- applying
static void apply_fog(const Style* st, float t) {
  if (!g_fog_ok || !g_fog.valid) return;
  if (st && t > 0.001f && st->fog_den > 0) {
    set_b(rs_set_fog, true);
    set_i(rs_set_fmode, 3);  // FogMode.ExponentialSquared
    Col c = {lerpf(g_fog.col.r, st->fog_col[0], t), lerpf(g_fog.col.g, st->fog_col[1], t),
             lerpf(g_fog.col.b, st->fog_col[2], t), 1};
    set_c(rs_set_fcol, c);
    set_f(rs_set_fden, lerpf(g_fog.fog ? g_fog.den : 0.0f, st->fog_den, t));
  } else {
    set_b(rs_set_fog, g_fog.fog);
    set_i(rs_set_fmode, g_fog.mode);
    set_c(rs_set_fcol, g_fog.col);
    set_f(rs_set_fden, g_fog.den);
  }
}

// st = nullptr or t = 0 puts the original look back.
static void apply(const Style* st, float t) {
  bool on = st && t > 0.001f;
  if (!on && !g_touched) return;
  for (size_t i = 0; i < g_entries.size();) {
    Entry& e = g_entries[i];
    Col c = e.orig;
    if (on) {
      const float* m = e.sky ? st->sky : st->world;
      c.r *= lerpf(1, m[0], t);
      c.g *= lerpf(1, m[1], t);
      c.b *= lerpf(1, m[2], t);
    }
    call(m_set_col, e.mat, {g_prop[e.prop], &c});
    if (g_exc) {  // the material was destroyed, forget it
      g_entries.erase(g_entries.begin() + static_cast<long>(i));
      continue;
    }
    ++i;
  }
  g_touched = on;
  apply_fog(on ? st : nullptr, t);
}

static void apply_config() {
  if (!g_cfg.enabled || g_cfg.style == 0) apply(nullptr, 0);
  else apply(&kStyles[g_cfg.style], g_cfg.strength);
}

static void update(float dt) {
  if (g_dead) return;
  if (!g_ready) {
    g_t_retry += dt;
    if (g_t_retry < 3.0f) return;
    g_t_retry = 0;
    if (++g_tries > 12) {
      g_dead = true;
      say(BEARITE_LOG_ERROR, "giving up after %d tries", g_tries - 1);
      return;
    }
    if (!setup()) return;
  }
  g_t_cfg += dt;
  g_t_scan += dt;
  bool dirty = false;

  if (g_t_cfg >= 0.5f) {
    g_t_cfg = 0;
    Config c = read_config();
    if (!(c == g_cfg)) {
      say(BEARITE_LOG_INFO, "style: %s%s strength=%.2f", kStyles[c.style].name, c.enabled ? "" : " (off)", c.strength);
      g_cfg = c;
      dirty = true;
    }
  }

  if (g_t_scan >= 2.0f) {
    g_t_scan = 0;
    bool first = g_known.empty();
    if (scan()) dirty = true;
    if (first && !g_known.empty()) log_shaders();
    if (!dirty && g_touched) apply_fog(&kStyles[g_cfg.style], g_cfg.strength);  // game may reset fog
  }

  if (dirty) apply_config();
}

static void unload() {
  if (g_ready && g_touched) apply(nullptr, 0);
}

}  // namespace vis

// ============================================================ 4. exports

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  if (api && api->mod_dir) vis::g_dir = api->mod_dir;
  say(BEARITE_LOG_INFO, "Visual Styles 0.2 loaded, mod dir: %s", vis::g_dir.c_str());
  return 0;
}

BEARITE_EXPORT void bearite_on_update(float dt) { vis::update(dt); }

BEARITE_EXPORT void bearite_on_unload(void) { vis::unload(); }
}  // extern "C"
