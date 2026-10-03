// visuals.cpp: "Visual Styles" mod for Super Bear Adventure.
// Switches the look of the game: 0 = original day, 1 = sunset, 2 = night, 3 = pastel.
//
// It only uses standard Unity lighting: RenderSettings (ambient light, fog), the sun Light
// (color, intensity, angle) and the skybox Material (tint, exposure). Everything is read once
// from the game ("original"), and every style is computed from that original, so style 0
// always brings the game back exactly as it was.
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

// ============================================================ 1. il2cpp =====
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

// libil2cpp.so is usually loaded with RTLD_LOCAL, so look in its own handle first.
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
         A.string_new && A.class_get_type && A.type_get_object && A.object_unbox && A.gchandle_new;
  if (!A.ok) say(BEARITE_LOG_ERROR, "il2cpp API is incomplete (libil2cpp handle: %s)", g_lib ? "yes" : "no");
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

// Method by name and argument count, looking through base classes.
// p0 (optional) = type name of the first parameter, e.g. "System.String", to tell overloads apart.
static void* find_method(void* klass, const char* name, int argc, const char* p0 = nullptr) {
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
  if (!method) return nullptr;
  void* buf[8];
  size_t n = 0;
  for (void* p : args) if (n < 8) buf[n++] = p;
  void* exc = nullptr;
  void* r = A.runtime_invoke(method, obj, n ? buf : nullptr, &exc);
  g_exc = exc != nullptr;
  return g_exc ? nullptr : r;
}

static void* keep(void* obj) {  // keep a managed object alive forever
  if (obj) A.gchandle_new(obj, 0);
  return obj;
}

static void* type_object(void* klass) { return keep(A.type_get_object(A.class_get_type(klass))); }

static uint8_t* arr_data(void* arr) { return static_cast<uint8_t*>(arr) + 4 * sizeof(void*); }
static size_t arr_len(void* arr) {
  return arr ? *reinterpret_cast<size_t*>(static_cast<uint8_t*>(arr) + 3 * sizeof(void*)) : 0;
}

static std::string str_utf8(void* s) {  // managed string -> ASCII (others become '?')
  std::string out;
  if (!s || !A.string_chars || !A.string_length) return out;
  const uint16_t* c = A.string_chars(s);
  int n = A.string_length(s);
  for (int i = 0; i < n && i < 120; ++i) out += c[i] < 128 ? static_cast<char>(c[i]) : '?';
  return out;
}

}  // namespace il

// =========================================================== 2. styles =====

struct Col { float r = 0, g = 0, b = 0, a = 1; };
struct V3 { float x = 0, y = 0, z = 0; };

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static Col lerpc(const Col& a, const Col& b, float t) {
  return {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t), a.a};
}
// c * lerp(1, m, t): a style is a tint that multiplies the original colors.
static Col mulc(const Col& c, const float m[3], float t) {
  return {c.r * lerpf(1, m[0], t), c.g * lerpf(1, m[1], t), c.b * lerpf(1, m[2], t), c.a};
}
static bool col_eq(const Col& a, const Col& b) {
  return std::fabs(a.r - b.r) < 0.002f && std::fabs(a.g - b.g) < 0.002f && std::fabs(a.b - b.b) < 0.002f;
}

struct Style {
  const char* name;
  float sun[3];      // sun color multiplier
  float sun_i;       // sun intensity multiplier
  bool set_euler;    // rotate the sun?
  V3 euler;          // sun angle (X = height above the horizon, Y = direction)
  float amb[3];      // ambient light multiplier
  float sky[3];      // skybox tint multiplier
  float sky_exp;     // skybox exposure multiplier
  Col fog_col;       // fog color
  float fog_den;     // fog density (0 = keep fog off)
};

// Index = value of the "style" setting. 0 = the game as it was.
static const Style kStyles[4] = {
    {"day (original)", {1, 1, 1}, 1, false, {}, {1, 1, 1}, {1, 1, 1}, 1, {}, 0},
    {"sunset", {1.0f, 0.58f, 0.33f}, 1.05f, true, {10, -35, 0}, {1.05f, 0.62f, 0.48f},
     {1.35f, 0.8f, 0.6f}, 1.0f, {0.95f, 0.5f, 0.35f, 1}, 0.004f},
    {"night", {0.35f, 0.5f, 1.0f}, 0.3f, true, {55, -30, 0}, {0.12f, 0.17f, 0.45f},
     {0.25f, 0.35f, 0.9f}, 0.3f, {0.03f, 0.05f, 0.16f, 1}, 0.012f},
    {"pastel", {1.0f, 0.96f, 0.92f}, 0.95f, false, {}, {1.25f, 1.25f, 1.35f},
     {1.05f, 1.1f, 1.25f}, 1.15f, {0.85f, 0.9f, 1.0f, 1}, 0.006f},
};

// ============================================================ 3. the mod ====

namespace vis {

using namespace il;

struct Config {
  bool enabled = true;
  int style = 0;
  float strength = 1;
  bool operator==(const Config& o) const {
    return enabled == o.enabled && style == o.style && strength == o.strength;
  }
};

static std::string g_dir;
static Config g_cfg, g_applied;
static bool g_ready = false, g_failed = false, g_captured = false;
static float g_t_cfg = 10, g_t_poll = 10, g_t_find = 10;

// classes and methods
static void *k_light, *k_mat, *t_light;
static void *rs_get_amb, *rs_set_amb, *rs_get_amode, *rs_set_amode, *rs_get_fog, *rs_set_fog,
    *rs_get_fmode, *rs_set_fmode, *rs_get_fcol, *rs_set_fcol, *rs_get_fden, *rs_set_fden,
    *rs_get_sky, *rs_get_sun;
static void *l_get_col, *l_set_col, *l_get_int, *l_set_int, *l_get_xf, *l_get_type;
static void *xf_get_euler, *xf_set_euler;
static void *m_has, *m_get_col, *m_set_col, *m_get_f, *m_set_f, *m_get_shader;
static void *o_get_name, *o_find_all;
static bool o_find_by = false;

// Originals, read from the game.
struct RsOrig { bool valid = false; Col amb; int amode = 0; bool fog = false; int fmode = 1; Col fcol; float fden = 0; };
static RsOrig g_rs;
static Col g_last_amb;
static void *g_sun = nullptr, *g_sky = nullptr;
static Col g_sun_col, g_sky_tint;
static float g_sun_int = 1, g_sky_exp = 1;
static V3 g_sun_eul;
static const char *g_tint_prop = nullptr, *g_exp_prop = nullptr;
static void* g_found_sun = nullptr;

// ---- typed calls ----
static Col get_col(void* m, void* obj, std::initializer_list<void*> a = {}) {
  Col c;
  if (void* r = call(m, obj, a)) memcpy(&c, A.object_unbox(r), sizeof(Col));
  return c;
}
static V3 get_v3(void* m, void* obj) {
  V3 v;
  if (void* r = call(m, obj)) memcpy(&v, A.object_unbox(r), sizeof(V3));
  return v;
}
static float get_f(void* m, void* obj, std::initializer_list<void*> a = {}) {
  void* r = call(m, obj, a);
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
static void set_col(void* m, void* obj, Col c) { call(m, obj, {&c}); }
static void set_v3(void* m, void* obj, V3 v) { call(m, obj, {&v}); }
static void set_f(void* m, void* obj, float f) { call(m, obj, {&f}); }
static void set_i(void* m, void* obj, int i) { call(m, obj, {&i}); }
static void set_b(void* m, void* obj, bool b) { uint8_t v = b ? 1 : 0; call(m, obj, {&v}); }

static bool has_prop(void* mat, const char* name) { return get_b(m_has, mat, {A.string_new(name)}); }
static Col mat_col(void* mat, const char* name) { return get_col(m_get_col, mat, {A.string_new(name)}); }
static float mat_f(void* mat, const char* name) { return get_f(m_get_f, mat, {A.string_new(name)}); }
static void set_mat_col(void* mat, const char* name, Col c) { call(m_set_col, mat, {A.string_new(name), &c}); }
static void set_mat_f(void* mat, const char* name, float f) { call(m_set_f, mat, {A.string_new(name), &f}); }

static std::string obj_name(void* o) { return o ? str_utf8(call(o_get_name, o)) : std::string("(none)"); }

// ---- settings ----
// settings.json in the mod folder first (always fresh), then the loader API.
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
  if (get_setting_num(file, "style", v)) c.style = static_cast<int>(std::lround(v));
  if (get_setting_num(file, "strength", v)) c.strength = static_cast<float>(v);
  c.style = std::min(3, std::max(0, c.style));
  c.strength = std::min(1.0f, std::max(0.0f, c.strength));
  return c;
}

// ---- setup ----
static bool need(void* p, const char* what) {
  if (!p) { say(BEARITE_LOG_ERROR, "setup failed: %s not found", what); g_failed = true; return false; }
  return true;
}

static bool setup() {
  if (!il::init()) { g_failed = true; return false; }
  const char* core = "UnityEngine.CoreModule";
  void* k_rs = find_class(core, "UnityEngine", "RenderSettings");
  k_light = find_class(core, "UnityEngine", "Light");
  k_mat = find_class(core, "UnityEngine", "Material");
  void* k_xf = find_class(core, "UnityEngine", "Transform");
  void* k_obj = find_class(core, "UnityEngine", "Object");
  void* k_shader = find_class(core, "UnityEngine", "Shader");
  if (!need(k_rs, "UnityEngine.RenderSettings") || !need(k_light, "UnityEngine.Light") ||
      !need(k_mat, "UnityEngine.Material") || !need(k_xf, "UnityEngine.Transform") ||
      !need(k_obj, "UnityEngine.Object"))
    return false;
  t_light = type_object(k_light);

#define RS(var, name, argc) var = find_method(k_rs, name, argc)
  RS(rs_get_amb, "get_ambientLight", 0);   RS(rs_set_amb, "set_ambientLight", 1);
  RS(rs_get_amode, "get_ambientMode", 0);  RS(rs_set_amode, "set_ambientMode", 1);
  RS(rs_get_fog, "get_fog", 0);            RS(rs_set_fog, "set_fog", 1);
  RS(rs_get_fmode, "get_fogMode", 0);      RS(rs_set_fmode, "set_fogMode", 1);
  RS(rs_get_fcol, "get_fogColor", 0);      RS(rs_set_fcol, "set_fogColor", 1);
  RS(rs_get_fden, "get_fogDensity", 0);    RS(rs_set_fden, "set_fogDensity", 1);
  RS(rs_get_sky, "get_skybox", 0);         RS(rs_get_sun, "get_sun", 0);
#undef RS
  l_get_col = find_method(k_light, "get_color", 0);
  l_set_col = find_method(k_light, "set_color", 1);
  l_get_int = find_method(k_light, "get_intensity", 0);
  l_set_int = find_method(k_light, "set_intensity", 1);
  l_get_xf = find_method(k_light, "get_transform", 0);
  l_get_type = find_method(k_light, "get_type", 0);
  xf_get_euler = find_method(k_xf, "get_eulerAngles", 0);
  xf_set_euler = find_method(k_xf, "set_eulerAngles", 1);
  m_has = find_method(k_mat, "HasProperty", 1, "System.String");
  m_get_col = find_method(k_mat, "GetColor", 1, "System.String");
  m_set_col = find_method(k_mat, "SetColor", 2, "System.String");
  m_get_f = find_method(k_mat, "GetFloat", 1, "System.String");
  m_set_f = find_method(k_mat, "SetFloat", 2, "System.String");
  m_get_shader = find_method(k_mat, "get_shader", 0);
  o_get_name = find_method(k_obj, "get_name", 0);
  o_find_all = find_method(k_obj, "FindObjectsOfType", 1, "System.Type");
  if (!o_find_all) { o_find_all = find_method(k_obj, "FindObjectsByType", 2, "System.Type"); o_find_by = o_find_all != nullptr; }

  if (!need(rs_get_amb, "RenderSettings.ambientLight") || !need(rs_set_amb, "RenderSettings.set_ambientLight") ||
      !need(rs_get_amode, "RenderSettings.ambientMode") || !need(rs_get_fog, "RenderSettings.fog") ||
      !need(rs_get_fcol, "RenderSettings.fogColor") || !need(rs_get_fden, "RenderSettings.fogDensity") ||
      !need(rs_get_sky, "RenderSettings.skybox") || !need(l_get_col, "Light.color") ||
      !need(l_set_col, "Light.set_color") || !need(l_get_int, "Light.intensity") ||
      !need(l_set_int, "Light.set_intensity") || !need(l_get_xf, "Light.transform") ||
      !need(xf_get_euler, "Transform.eulerAngles") || !need(xf_set_euler, "Transform.set_eulerAngles") ||
      !need(m_set_col, "Material.SetColor") || !need(m_has, "Material.HasProperty"))
    return false;

  // Which render pipeline? (only for the log, helps to tune the styles)
  if (void* k_gs = find_class(core, "UnityEngine.Rendering", "GraphicsSettings")) {
    void* m = find_method(k_gs, "get_currentRenderPipeline", 0);
    if (!m) m = find_method(k_gs, "get_renderPipelineAsset", 0);
    void* rp = m ? call(m, nullptr) : nullptr;
    say(BEARITE_LOG_INFO, "render pipeline: %s", rp ? obj_name(rp).c_str() : "built-in");
  }
  (void)k_shader;
  say(BEARITE_LOG_INFO, "setup done");
  g_ready = true;
  return true;
}

// ---- finding objects ----
// RenderSettings.sun if the scene sets it, otherwise the first directional Light.
static void* find_sun() {
  if (void* s = call(rs_get_sun, nullptr)) return s;
  if (!o_find_all || !t_light) return nullptr;
  if (g_found_sun && g_t_find < 2.0f) return g_found_sun;
  g_t_find = 0;
  int sort_mode = 0;
  void* arr = o_find_by ? call(o_find_all, nullptr, {t_light, &sort_mode}) : call(o_find_all, nullptr, {t_light});
  g_found_sun = nullptr;
  size_t n = arr_len(arr);
  for (size_t i = 0; i < n; ++i) {
    void* l = reinterpret_cast<void**>(arr_data(arr))[i];
    if (l && get_i(l_get_type, l) == 1) { g_found_sun = l; break; }  // LightType.Directional
  }
  return g_found_sun;
}

// Reads the "original" look from the game. keep_rs = the RenderSettings values are ours
// (a new sun/skybox appeared in the same scene), so keep the originals we already have.
static void capture(void* sun, void* sky, bool keep_rs) {
  if (!(keep_rs && g_rs.valid)) {
    g_rs.amb = get_col(rs_get_amb, nullptr);
    g_rs.amode = get_i(rs_get_amode, nullptr);
    g_rs.fog = get_b(rs_get_fog, nullptr);
    g_rs.fmode = get_i(rs_get_fmode, nullptr);
    g_rs.fcol = get_col(rs_get_fcol, nullptr);
    g_rs.fden = get_f(rs_get_fden, nullptr);
    g_rs.valid = true;
  }
  g_sun = keep(sun);
  if (sun) {
    g_sun_col = get_col(l_get_col, sun);
    g_sun_int = get_f(l_get_int, sun);
    g_sun_eul = get_v3(xf_get_euler, call(l_get_xf, sun));
  }
  g_sky = keep(sky);
  g_tint_prop = g_exp_prop = nullptr;
  std::string shader = "?";
  if (sky) {
    if (has_prop(sky, "_SkyTint")) g_tint_prop = "_SkyTint";
    else if (has_prop(sky, "_Tint")) g_tint_prop = "_Tint";
    if (has_prop(sky, "_Exposure")) g_exp_prop = "_Exposure";
    if (g_tint_prop) g_sky_tint = mat_col(sky, g_tint_prop);
    if (g_exp_prop) g_sky_exp = mat_f(sky, g_exp_prop);
    if (m_get_shader) shader = obj_name(call(m_get_shader, sky));
  }
  say(BEARITE_LOG_INFO, "scene: sun=%s intensity=%.2f | skybox=%s shader=%s tint=%s exposure=%s",
      sun ? obj_name(sun).c_str() : "NONE", g_sun_int, sky ? obj_name(sky).c_str() : "NONE", shader.c_str(),
      g_tint_prop ? g_tint_prop : "-", g_exp_prop ? g_exp_prop : "-");
  say(BEARITE_LOG_INFO, "scene: ambientMode=%d fog=%d fogMode=%d fogDensity=%.4f", g_rs.amode, g_rs.fog ? 1 : 0,
      g_rs.fmode, g_rs.fden);
}

// ---- applying ----
// st = nullptr or t = 0 puts the original look back.
static void apply(const Style* st, float t) {
  if (!g_rs.valid) return;
  bool on = st && t > 0.001f;

  // ambient light
  if (on) {
    Col base = g_rs.amb;
    if (g_rs.amode != 3) base = {0.55f, 0.55f, 0.55f, 1};  // ambient came from the skybox: start from gray
    set_i(rs_set_amode, nullptr, 3);                       // AmbientMode.Flat
    g_last_amb = {base.r * lerpf(1, st->amb[0], t), base.g * lerpf(1, st->amb[1], t),
                  base.b * lerpf(1, st->amb[2], t), 1};
    set_col(rs_set_amb, nullptr, g_last_amb);
  } else {
    set_i(rs_set_amode, nullptr, g_rs.amode);
    set_col(rs_set_amb, nullptr, g_rs.amb);
    g_last_amb = g_rs.amb;
  }

  // fog
  if (on && st->fog_den > 0) {
    set_b(rs_set_fog, nullptr, true);
    set_i(rs_set_fmode, nullptr, 3);  // FogMode.ExponentialSquared
    set_col(rs_set_fcol, nullptr, g_rs.fog ? lerpc(g_rs.fcol, st->fog_col, t) : st->fog_col);
    set_f(rs_set_fden, nullptr, lerpf(g_rs.fog ? g_rs.fden : 0.0f, st->fog_den, t));
  } else {
    set_b(rs_set_fog, nullptr, g_rs.fog);
    set_i(rs_set_fmode, nullptr, g_rs.fmode);
    set_col(rs_set_fcol, nullptr, g_rs.fcol);
    set_f(rs_set_fden, nullptr, g_rs.fden);
  }

  // sun
  if (g_sun) {
    set_col(l_set_col, g_sun, on ? mulc(g_sun_col, st->sun, t) : g_sun_col);
    bool bad = g_exc;
    set_f(l_set_int, g_sun, on ? g_sun_int * lerpf(1, st->sun_i, t) : g_sun_int);
    V3 e = g_sun_eul;
    if (on && st->set_euler)
      e = {lerpf(g_sun_eul.x, st->euler.x, t), lerpf(g_sun_eul.y, st->euler.y, t), lerpf(g_sun_eul.z, st->euler.z, t)};
    set_v3(xf_set_euler, call(l_get_xf, g_sun), e);
    if (bad || g_exc) { g_sun = nullptr; g_found_sun = nullptr; g_captured = false; }  // destroyed: look again
  }

  // skybox
  if (g_sky) {
    if (g_tint_prop) set_mat_col(g_sky, g_tint_prop, on ? mulc(g_sky_tint, st->sky, t) : g_sky_tint);
    if (g_exp_prop) set_mat_f(g_sky, g_exp_prop, on ? g_sky_exp * lerpf(1, st->sky_exp, t) : g_sky_exp);
  }
}

static void apply_config() {
  if (!g_cfg.enabled || g_cfg.style == 0) apply(nullptr, 0);
  else apply(&kStyles[g_cfg.style], g_cfg.strength);
}

static void update(float dt) {
  if (g_failed) return;
  const BeariteApi* api = bearite::api();
  if (!api || !BEARITE_API_HAS(api, il2cpp_ready) || !api->il2cpp_ready(api)) return;
  if (!g_ready && !setup()) return;
  g_t_cfg += dt;
  g_t_poll += dt;
  g_t_find += dt;

  if (g_t_cfg >= 0.5f) {
    g_t_cfg = 0;
    Config c = read_config();
    if (!(c == g_cfg) || !g_captured) {
      if (c.style != g_cfg.style || c.enabled != g_cfg.enabled)
        say(BEARITE_LOG_INFO, "style: %s%s", kStyles[c.style].name, c.enabled ? "" : " (mod disabled)");
      g_cfg = c;
      g_t_poll = 10;  // apply right away
    }
  }

  if (g_t_poll >= 0.25f) {
    g_t_poll = 0;
    void* sun = find_sun();
    void* sky = call(rs_get_sky, nullptr);
    if (!g_captured || sun != g_sun || sky != g_sky) {
      bool ours = g_rs.valid && col_eq(get_col(rs_get_amb, nullptr), g_last_amb);
      capture(sun, sky, ours);
      g_captured = true;
    }
    // Re-apply every poll: the game may reset the lighting by itself.
    apply_config();
    g_applied = g_cfg;
  }
}

static void unload() {
  if (g_ready && g_captured) apply(nullptr, 0);
}

}  // namespace vis

// ============================================================ 4. exports =====

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  if (api && api->mod_dir) vis::g_dir = api->mod_dir;
  say(BEARITE_LOG_INFO, "Visual Styles loaded, mod dir: %s", vis::g_dir.c_str());
  return 0;
}

BEARITE_EXPORT void bearite_on_update(float dt) { vis::update(dt); }

BEARITE_EXPORT void bearite_on_unload(void) { vis::unload(); }

}  // extern "C"
