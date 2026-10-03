// visuals.cpp: "Visual Styles" mod for Super Bear Adventure (v0.3).
//
// The game's shaders ignore Unity lighting, so changing lights only recolours
// the sky. This mod therefore:
//   1. tints EVERY colour property of EVERY material (found per shader, so
//      flowers, sprites and characters are covered too),
//   2. adds fog,
//   3. turns on real shadows: sun light shadows + shadow casting for the
//      player (skinned meshes and anything whose root is called player/bear/hero),
//   4. for sunset/night rotates the sun so the shadows get long.
// Style 0 puts everything back exactly as it was.
//
// Every step can fail on its own without stopping the mod. What it finds
// (or does not find) is written to bearite.log.
//
// Exports for the Bearite loader: bearite_on_load, bearite_on_update, bearite_on_unload.

#include <algorithm>
#include <cctype>
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
  void* (*object_new)(void*);
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
  L(object_new, "il2cpp_object_new");
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
struct V3 { float x = 0, y = 0, z = 0; };

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float lerp_angle(float a, float b, float t) {
  float d = std::fmod(b - a, 360.0f);
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  return a + d * t;
}

struct Style {
  const char* name;
  float world[3];    // colour multiplier for world, characters, objects
  float sky[3];      // colour multiplier for the sky material
  float mix_col[3];  // washed-out colour that colours are blended towards
  float mix;         // 0 = only multiply, 1 = fully mix_col
  float fog_col[3];  // fog colour
  float fog_den;     // fog density (0 = do not touch fog)
  bool set_sun;      // rotate the sun (longer shadows)?
  V3 sun_euler;      // sun angle: X = height above the horizon, Y = direction
};

// Index = value of the "style" setting. 0 = the game as it was.
static const Style kStyles[4] = {
    {"day (original)", {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 0.0f, {1, 1, 1}, 0.0f, false, {}},
    {"sunset", {1.12f, 0.76f, 0.56f}, {1.25f, 0.70f, 0.60f}, {1.0f, 0.6f, 0.4f}, 0.0f,
     {1.0f, 0.55f, 0.32f}, 0.004f, true, {16, -35, 0}},
    {"night", {0.30f, 0.40f, 0.80f}, {0.20f, 0.26f, 0.65f}, {0.1f, 0.15f, 0.4f}, 0.0f,
     {0.04f, 0.06f, 0.18f}, 0.010f, true, {60, -30, 0}},
    {"pastel", {1.02f, 1.0f, 1.08f}, {1.1f, 1.1f, 1.3f}, {0.96f, 0.91f, 1.0f}, 0.30f,
     {0.85f, 0.90f, 1.0f}, 0.006f, false, {}},
};

// ============================================================ 3. mod

namespace vis {

using namespace il;

struct Config {
  bool enabled = true;
  int style = 0;
  float strength = 1;
  bool shadows = true;
  bool operator==(const Config& o) const {
    return enabled == o.enabled && style == o.style && shadows == o.shadows &&
           std::fabs(strength - o.strength) < 0.001f;
  }
};

static std::string g_dir;
static Config g_cfg;
static bool g_ready = false, g_dead = false, g_touched = false;
static int g_tries = 0;
static float g_t_retry = 10, g_t_cfg = 10, g_t_scan = 10;

// classes and methods
static void *k_mat, *t_mat, *t_rend, *t_skin, *t_light;
static void *m_has, *m_get_col, *m_set_col, *m_get_shader;
static void *o_get_name, *find_res, *find_scene;
static void *sh_count, *sh_pname, *sh_ptype;
static void *rs_get_fog, *rs_set_fog, *rs_get_fmode, *rs_set_fmode, *rs_get_fcol, *rs_set_fcol,
    *rs_get_fden, *rs_set_fden;
static void *comp_get_xf, *xf_get_root, *xf_get_euler, *xf_set_euler;
static void *l_get_type, *l_get_sh, *l_set_sh, *l_get_sstr, *l_set_sstr;
static void *r_get_cast, *r_set_cast;
static void *qs_get_sh, *qs_set_sh, *qs_get_dist, *qs_set_dist;
static bool g_fog_ok = false, g_generic = false, g_shadow_ok = false, g_qs_ok = false;

static void* g_exposure_str = nullptr;
static const char* kLegacy[] = {"_Color", "_BaseColor", "_TintColor", "_Tint", "_SkyTint", "_MainColor"};
static std::vector<void*> g_legacy;

struct Entry {
  void* mat;
  void* prop;  // managed string
  Col orig;
  bool sky;
};
static std::vector<Entry> g_entries;
static std::map<void*, int> g_known;
static std::map<std::string, std::vector<void*>> g_shader_props;
static std::map<std::string, int> g_shader_tinted, g_shader_noprop;
static int g_skipped = 0;

struct FogOrig { bool valid = false; bool fog = false; int mode = 0; Col col; float den = 0; };
static FogOrig g_fog;

// ---- typed calls
static Col get_col(void* m, void* obj, void* arg) {
  Col c;
  if (void* r = call(m, obj, {arg})) memcpy(&c, A.object_unbox(r), sizeof(Col));
  return c;
}
static V3 get_v3(void* m, void* obj) {
  V3 v;
  if (void* r = call(m, obj)) memcpy(&v, A.object_unbox(r), sizeof(V3));
  return v;
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
static int call_i1(void* m, void* obj, int a) {  // method(int) -> enum/int
  void* r = call(m, obj, {&a});
  return r ? *static_cast<int*>(A.object_unbox(r)) : -1;
}
static void* call_p1(void* m, void* obj, int a) { return call(m, obj, {&a}); }

static void seti_o(void* m, void* obj, int i) { call(m, obj, {&i}); }
static void setf_o(void* m, void* obj, float f) { call(m, obj, {&f}); }
static void setb_o(void* m, void* obj, bool b) { uint8_t v = b ? 1 : 0; call(m, obj, {&v}); }
static void setv_o(void* m, void* obj, V3 v) { call(m, obj, {&v}); }

static std::string obj_name(void* o) { return o ? str_utf8(call(o_get_name, o)) : std::string(); }

static std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

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
  if (get_setting_num(file, "shadows", v)) c.shadows = v != 0;
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
  void* k_shader = find_class(core, "UnityEngine", "Shader");
  void* k_rend = find_class(core, "UnityEngine", "Renderer");
  void* k_skin = find_class(core, "UnityEngine", "SkinnedMeshRenderer");
  void* k_comp = find_class(core, "UnityEngine", "Component");
  void* k_xf = find_class(core, "UnityEngine", "Transform");
  void* k_light = find_class(core, "UnityEngine", "Light");
  void* k_qs = find_class(core, "UnityEngine", "QualitySettings");
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

  find_res = find_method(k_res, "FindObjectsOfTypeAll", 1, "System.Type");
  const char* find_src = "Resources.FindObjectsOfTypeAll";
  if (!find_res) { find_res = find_method(k_obj, "FindObjectsOfType", 1, "System.Type"); find_src = "Object.FindObjectsOfType"; }
  find_scene = find_method(k_obj, "FindObjectsOfType", 1, "System.Type");

  // generic colour-property discovery
  sh_count = find_method(k_shader, "GetPropertyCount", 0);
  sh_pname = find_method(k_shader, "GetPropertyName", 1, "System.Int32");
  sh_ptype = find_method(k_shader, "GetPropertyType", 1, "System.Int32");
  g_generic = sh_count && sh_pname && sh_ptype;

  say(BEARITE_LOG_INFO, "methods: HasProperty=%d GetColor=%d SetColor=%d get_shader=%d get_name=%d find=%s(%d) generic=%d",
      m_has != nullptr, m_get_col != nullptr, m_set_col != nullptr, m_get_shader != nullptr,
      o_get_name != nullptr, find_src, find_res != nullptr, g_generic);

  if (!m_has || !m_get_col || !m_set_col || !o_get_name || !find_res) {
    say(BEARITE_LOG_ERROR, "setup failed: a required Material method was not found");
    return false;
  }

  for (const char* p : kLegacy) g_legacy.push_back(keep(A.string_new(p)));
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

  // shadows are optional too
  comp_get_xf = find_method(k_comp, "get_transform", 0);
  xf_get_root = find_method(k_xf, "get_root", 0);
  xf_get_euler = find_method(k_xf, "get_eulerAngles", 0);
  xf_set_euler = find_method(k_xf, "set_eulerAngles", 1);
  l_get_type = find_method(k_light, "get_type", 0);
  l_get_sh = find_method(k_light, "get_shadows", 0);
  l_set_sh = find_method(k_light, "set_shadows", 1);
  l_get_sstr = find_method(k_light, "get_shadowStrength", 0);
  l_set_sstr = find_method(k_light, "set_shadowStrength", 1);
  r_get_cast = find_method(k_rend, "get_shadowCastingMode", 0);
  r_set_cast = find_method(k_rend, "set_shadowCastingMode", 1);
  qs_get_sh = find_method(k_qs, "get_shadows", 0);
  qs_set_sh = find_method(k_qs, "set_shadows", 1);
  qs_get_dist = find_method(k_qs, "get_shadowDistance", 0);
  qs_set_dist = find_method(k_qs, "set_shadowDistance", 1);
  if (k_rend) t_rend = type_object(k_rend);
  if (k_skin) t_skin = type_object(k_skin);
  if (k_light) t_light = type_object(k_light);
  g_shadow_ok = find_scene && t_rend && t_light && comp_get_xf && xf_get_root && l_get_type &&
                l_set_sh && l_set_sstr && r_get_cast && r_set_cast;
  g_qs_ok = qs_get_sh && qs_set_sh && qs_get_dist && qs_set_dist;
  say(BEARITE_LOG_INFO, "shadows: ok=%d quality=%d skinned=%d sun_rotate=%d",
      g_shadow_ok, g_qs_ok, t_skin != nullptr, xf_set_euler != nullptr && xf_get_euler != nullptr);

  // what the render pipeline is (helps to understand shadows)
  if (void* k_gs = find_class(core, "UnityEngine.Rendering", "GraphicsSettings")) {
    void* m = find_method(k_gs, "get_currentRenderPipeline", 0);
    if (!m) m = find_method(k_gs, "get_renderPipelineAsset", 0);
    void* rp = m ? call(m, nullptr) : nullptr;
    say(BEARITE_LOG_INFO, "render pipeline: %s", rp ? obj_name(rp).c_str() : "built-in");
  }

  say(BEARITE_LOG_INFO, "setup done");
  g_ready = true;
  return true;
}

// ---- finding materials
static bool skip_shader(const std::string& s) {
  static const char* bad[] = {"UI/", "UI-", "TextMeshPro", "TMP", "Hidden/", "GUI", "Text"};
  for (const char* b : bad) if (has(s, b)) return true;
  return false;
}

static bool skip_prop(const std::string& p) {
  std::string l = lower(p);
  return has(l, "spec") || has(l, "emiss") || has(l, "outline") || has(l, "reflect") || has(l, "rim");
}
