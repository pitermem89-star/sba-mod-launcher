// visuals.cpp: "Visual Styles" mod for Super Bear Adventure (v0.7).
//
// v0.7: ad removal moved to its own mod ("No Ads", examples/no-ads); colours are softer; a flat colour grade covers everything the material tint misses (paths, snow, signs, portals).
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
    {"sunset", {1.0f, 0.85f, 0.70f}, {1.0f, 0.75f, 0.66f}, {1.0f, 0.6f, 0.4f}, 0.0f,
     {1.0f, 0.55f, 0.32f}, 0.003f, true, {16, -35, 0}},
    {"night", {0.50f, 0.58f, 0.88f}, {0.25f, 0.32f, 0.70f}, {0.1f, 0.15f, 0.4f}, 0.0f,
     {0.04f, 0.06f, 0.18f}, 0.006f, true, {60, -30, 0}},
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

static const std::vector<void*>& color_props(void* shader, const std::string& sname) {
  auto it = g_shader_props.find(sname);
  if (it != g_shader_props.end()) return it->second;
  std::vector<void*> out;
  int n = get_i(sh_count, shader);
  for (int i = 0; i < n && i < 64; ++i) {
    if (call_i1(sh_ptype, shader, i) != 0) continue;  // 0 = Color
    std::string pn = str_utf8(call_p1(sh_pname, shader, i));
    if (pn.empty() || skip_prop(pn)) continue;
    out.push_back(keep(A.string_new(pn.c_str())));
  }
  std::vector<void*>& slot = g_shader_props[sname];
  slot = out;
  return slot;
}

static void examine(void* mat) {
  void* sh = m_get_shader ? call(m_get_shader, mat) : nullptr;
  if (g_exc) return;
  std::string sname = sh ? obj_name(sh) : std::string("?");
  if (skip_shader(sname)) { ++g_skipped; return; }
  bool sky = has(sname, "Skybox") || (g_exposure_str && get_b(m_has, mat, {g_exposure_str}));
  std::vector<void*> props;
  if (g_generic && sh) {
    props = color_props(sh, sname);
  } else {
    for (void* p : g_legacy) if (get_b(m_has, mat, {p})) props.push_back(p);
  }
  size_t before = g_entries.size();
  for (void* p : props) {
    Col c = get_col(m_get_col, mat, p);
    if (g_exc) continue;
    g_entries.push_back({mat, p, c, sky});
  }
  if (g_entries.size() != before) ++g_shader_tinted[sname + (sky ? " [sky]" : "")];
  else ++g_shader_noprop[sname];
}

static bool scan_materials() {
  void* arr = call(find_res, nullptr, {t_mat});
  if (g_exc || !arr) return false;
  size_t n = arr_len(arr);
  void** items = reinterpret_cast<void**>(arr_data(arr));
  size_t before = g_entries.size();
  for (size_t i = 0; i < n; ++i) {
    void* m = items[i];
    if (!m || g_known.count(m)) continue;
    keep(m);
    g_known[m] = 1;
    examine(m);
  }
  bool added = g_entries.size() != before;
  if (added) say(BEARITE_LOG_INFO, "materials: %zu found, %zu colours tracked, %d skipped (UI/text)",
                 g_known.size(), g_entries.size(), g_skipped);
  return added;
}

static void log_shaders() {
  int shown = 0;
  for (auto& kv : g_shader_tinted) {
    if (++shown > 12) break;
    say(BEARITE_LOG_INFO, "shader tinted: %s x%d", kv.first.c_str(), kv.second);
  }
  shown = 0;
  for (auto& kv : g_shader_noprop) {
    if (++shown > 8) break;
    say(BEARITE_LOG_INFO, "shader NO colour property: %s x%d", kv.first.c_str(), kv.second);
  }
}

// ---- applying colours and fog
static void apply_fog(const Style* st, float t) {
  if (!g_fog_ok || !g_fog.valid) return;
  if (st && t > 0.001f && st->fog_den > 0) {
    seti_o(rs_set_fmode, nullptr, 3);  // FogMode.ExponentialSquared
    setb_o(rs_set_fog, nullptr, true);
    Col c = {lerpf(g_fog.col.r, st->fog_col[0], t), lerpf(g_fog.col.g, st->fog_col[1], t),
             lerpf(g_fog.col.b, st->fog_col[2], t), 1};
    call(rs_set_fcol, nullptr, {&c});
    setf_o(rs_set_fden, nullptr, lerpf(g_fog.fog ? g_fog.den : 0.0f, st->fog_den, t));
  } else {
    setb_o(rs_set_fog, nullptr, g_fog.fog);
    seti_o(rs_set_fmode, nullptr, g_fog.mode);
    Col c = g_fog.col;
    call(rs_set_fcol, nullptr, {&c});
    setf_o(rs_set_fden, nullptr, g_fog.den);
  }
}

// A colour that was already in 0..1 stays in 0..1 (no blown-out neon yellow); HDR values are left alone.
static float tint(float o, float mul, float mc, float mix, float t) {
  float r = lerpf(o, o * mul * (1 - mix) + mc * mix, t);
  return o <= 1.0f ? std::min(1.0f, std::max(0.0f, r)) : r;
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
      c.r = tint(e.orig.r, m[0], st->mix_col[0], st->mix, t);
      c.g = tint(e.orig.g, m[1], st->mix_col[1], st->mix, t);
      c.b = tint(e.orig.b, m[2], st->mix_col[2], st->mix, t);
    }
    call(m_set_col, e.mat, {e.prop, &c});
    if (g_exc) {  // the material was destroyed, forget it
      g_entries.erase(g_entries.begin() + static_cast<long>(i));
      continue;
    }
    ++i;
  }
  g_touched = on;
  apply_fog(on ? st : nullptr, t);
}

// ---- shadows
struct RendOrig { void* r; int cast; };
static std::vector<RendOrig> g_rends;
static std::map<void*, int> g_rend_known;
static std::map<std::string, int> g_caster_names;

static void* g_sun = nullptr;
static int g_sun_sh = 0;
static float g_sun_str = 1;
static V3 g_sun_eul;
static bool g_sun_touched = false, g_rend_touched = false, g_qs_touched = false, g_qs_valid = false;
static int g_qs_sh = 0;
static float g_qs_dist = 0;

static std::string root_name(void* r) {
  void* tr = call(comp_get_xf, r);
  if (!tr) return std::string();
  void* root = xf_get_root ? call(xf_get_root, tr) : nullptr;
  return obj_name(root ? root : tr);
}

static bool caster_name(const std::string& n) {
  std::string l = lower(n);
  return has(l, "player") || has(l, "bear") || has(l, "hero") || has(l, "character");
}

static void add_caster(void* r, const std::string& name) {
  int cast = get_i(r_get_cast, r);
  if (g_exc) return;
  keep(r);
  g_rends.push_back({r, cast});
  int& n = g_caster_names[name];
  if (++n == 1 && g_caster_names.size() <= 14)
    say(BEARITE_LOG_INFO, "shadow caster: %s (was castMode=%d)", name.c_str(), cast);
}

static bool scan_renderers() {
  if (!g_shadow_ok) return false;
  size_t before = g_rends.size();
  // all skinned meshes are characters
  if (t_skin) {
    void* arr = call(find_scene, nullptr, {t_skin});
    if (!g_exc && arr) {
      size_t n = arr_len(arr);
      void** it = reinterpret_cast<void**>(arr_data(arr));
      for (size_t i = 0; i < n; ++i) {
        void* r = it[i];
        if (!r || g_rend_known.count(r)) continue;
        g_rend_known[r] = 1;
        add_caster(r, root_name(r));
      }
    }
  }
  // plain meshes that belong to the player, found by the root object's name
  void* arr = call(find_scene, nullptr, {t_rend});
  if (!g_exc && arr) {
    size_t n = arr_len(arr);
    void** it = reinterpret_cast<void**>(arr_data(arr));
    int budget = 400;
    for (size_t i = 0; i < n; ++i) {
      void* r = it[i];
      if (!r || g_rend_known.count(r)) continue;
      if (budget-- <= 0) break;
      g_rend_known[r] = 1;
      std::string name = root_name(r);
      if (caster_name(name)) add_caster(r, name);
    }
  }
  return g_rends.size() != before;
}

static void refresh_sun() {
  if (!g_shadow_ok) return;
  void* arr = call(find_scene, nullptr, {t_light});
  if (g_exc || !arr) return;
  size_t n = arr_len(arr);
  void** it = reinterpret_cast<void**>(arr_data(arr));
  void* sun = nullptr;
  for (size_t i = 0; i < n; ++i)
    if (it[i] && get_i(l_get_type, it[i]) == 1) { sun = it[i]; break; }  // 1 = Directional
  if (!sun) return;
  if (sun != g_sun) {
    g_sun = keep(sun);
    g_sun_touched = false;
    g_sun_sh = l_get_sh ? get_i(l_get_sh, sun) : 0;
    g_sun_str = l_get_sstr ? get_f(l_get_sstr, sun) : 1.0f;
    void* xf = call(comp_get_xf, sun);
    if (xf && xf_get_euler) g_sun_eul = get_v3(xf_get_euler, xf);
    say(BEARITE_LOG_INFO, "sun: %s shadows=%d strength=%.2f euler=(%.0f,%.0f,%.0f)", obj_name(sun).c_str(),
        g_sun_sh, g_sun_str, g_sun_eul.x, g_sun_eul.y, g_sun_eul.z);
  }
}

static void apply_shadows() {
  if (!g_shadow_ok) return;
  bool on = g_cfg.enabled && g_cfg.shadows;
  const Style* st = (g_cfg.enabled && g_cfg.style > 0) ? &kStyles[g_cfg.style] : nullptr;
  float t = g_cfg.strength;

  // quality settings
  if (g_qs_ok) {
    if (!g_qs_valid) {
      g_qs_sh = get_i(qs_get_sh, nullptr);
      g_qs_dist = get_f(qs_get_dist, nullptr);
      g_qs_valid = true;
      say(BEARITE_LOG_INFO, "quality: shadows=%d distance=%.1f", g_qs_sh, g_qs_dist);
    }
    if (on) {
      seti_o(qs_set_sh, nullptr, 2);  // ShadowQuality.All
      setf_o(qs_set_dist, nullptr, std::max(g_qs_dist, 45.0f));
      g_qs_touched = true;
    } else if (g_qs_touched) {
      seti_o(qs_set_sh, nullptr, g_qs_sh);
      setf_o(qs_set_dist, nullptr, g_qs_dist);
      g_qs_touched = false;
    }
  }

  // the sun
  if (g_sun) {
    void* xf = call(comp_get_xf, g_sun);
    if (g_exc) { g_sun = nullptr; g_sun_touched = false; }
    else {
      if (on) {
        seti_o(l_set_sh, g_sun, 2);  // LightShadows.Soft
        setf_o(l_set_sstr, g_sun, 0.85f);
        g_sun_touched = true;
      } else if (g_sun_touched) {
        seti_o(l_set_sh, g_sun, g_sun_sh);
        setf_o(l_set_sstr, g_sun, g_sun_str);
      }
      if (xf && xf_set_euler) {
        if (st && st->set_sun && t > 0.001f) {
          V3 e = {lerp_angle(g_sun_eul.x, st->sun_euler.x, t), lerp_angle(g_sun_eul.y, st->sun_euler.y, t),
                  lerp_angle(g_sun_eul.z, st->sun_euler.z, t)};
          setv_o(xf_set_euler, xf, e);
          g_sun_touched = true;
        } else if (g_sun_touched) {
          setv_o(xf_set_euler, xf, g_sun_eul);
        }
      }
      if (!on && !(st && st->set_sun)) g_sun_touched = false;
    }
  }

  // the player
  if (on || g_rend_touched) {
    for (size_t i = 0; i < g_rends.size();) {
      seti_o(r_set_cast, g_rends[i].r, on ? 1 : g_rends[i].cast);  // 1 = ShadowCastingMode.On
      if (g_exc) { g_rends.erase(g_rends.begin() + static_cast<long>(i)); continue; }
      ++i;
    }
    g_rend_touched = on;
  }
}

// ---- glow overlay: a soft glow and a vignette drawn over the world, under the HUD.
// Two UI images on their own overlay canvas (sorting order below the game's HUD).
struct V2 { float x = 0, y = 0; };
struct Rc { float x = 0, y = 0, w = 0, h = 0; };

struct GlowStyle {
  float glow_col[3];
  float glow_a;
  float rect[4];  // where the glow sits on screen (0..1, x0 y0 x1 y1, 0,0 = bottom left)
  float vig_col[3];
  float vig_a;
  float grade_col[3];  // flat colour over EVERYTHING (also objects whose material has no tintable colour)
  float grade_a;
};

static const GlowStyle kGlow[4] = {
    {{0, 0, 0}, 0.0f, {0, 0, 1, 1}, {0, 0, 0}, 0.0f, {0, 0, 0}, 0.0f},
    {{1.0f, 0.62f, 0.30f}, 0.38f, {0.30f, 0.10f, 1.15f, 1.25f}, {0.25f, 0.08f, 0.05f}, 0.35f,
     {1.0f, 0.50f, 0.22f}, 0.24f},
    {{0.35f, 0.45f, 1.0f}, 0.30f, {0.35f, 0.35f, 1.2f, 1.3f}, {0.0f, 0.01f, 0.08f}, 0.55f,
     {0.08f, 0.12f, 0.40f}, 0.34f},
    {{1.0f, 0.88f, 0.95f}, 0.30f, {-0.2f, -0.2f, 1.2f, 1.2f}, {1.0f, 0.93f, 0.97f}, 0.30f,
     {0.95f, 0.85f, 1.0f}, 0.16f},
};

static void *glow_root = nullptr, *glow_img = nullptr, *vig_img = nullptr, *glow_xf = nullptr,
            *vig_xf = nullptr, *grade_img = nullptr, *grade_xf = nullptr;
static void *go_ctor, *go_addc, *go_get_xf, *go_set_active, *xf_set_parent, *dont_destroy;
static void *rt_amin, *rt_amax, *rt_omin, *rt_omax, *cv_set_mode, *cv_set_order;
static void *img_set_sprite, *gr_set_color, *gr_set_ray;
static void *k_go_c = nullptr, *t_canvas = nullptr, *t_image = nullptr;
static bool g_glow_failed = false;
static float g_time = 0, g_t_glow = 0;
static int g_glow_style = -1;

static void* find_tex_ctor(void* k) {  // Texture2D(int, int, TextureFormat, bool)
  void* it = nullptr;
  while (void* m = A.class_get_methods(k, &it)) {
    if (strcmp(A.method_get_name(m), ".ctor") != 0 || A.method_get_param_count(m) != 4) continue;
    char* tn = A.type_get_name(A.method_get_param(m, 2));
    bool ok = tn && strcmp(tn, "UnityEngine.TextureFormat") == 0;
    if (tn && A.free_mem) A.free_mem(tn);
    if (ok) return m;
  }
  return nullptr;
}

// kind 0 = round glow (opaque in the middle, fades out), 1 = vignette (clear in the middle, opaque at the edges)
static void* make_sprite(void* k_tex, void* tex_ctor, void* tex_load, void* tex_apply, void* sprite_create, int kind) {
  const int N = 128;
  static uint8_t px[128 * 128 * 4];
  for (int y = 0; y < N; ++y) {
    for (int x = 0; x < N; ++x) {
      float nx = (x + 0.5f) / N * 2 - 1, ny = (y + 0.5f) / N * 2 - 1;
      float d = std::sqrt(nx * nx + ny * ny);
      float a;
      if (kind == 0) {
        a = std::max(0.0f, 1 - d);
        a = a * a;
      } else {
        a = std::min(1.0f, std::max(0.0f, (d - 0.55f) / 0.8f));
        a = a * a * (3 - 2 * a);
      }
      uint8_t* p = &px[(y * N + x) * 4];
      p[0] = p[1] = p[2] = 255;
      p[3] = static_cast<uint8_t>(a * 255.0f);
    }
  }
  void* tex = A.object_new(k_tex);
  int w = N, h = N, fmt = 4;  // TextureFormat.RGBA32
  uint8_t mip = 0;
  call(tex_ctor, tex, {&w, &h, &fmt, &mip});
  if (g_exc) return nullptr;
  void* ptr = px;
  int size = N * N * 4;
  call(tex_load, tex, {&ptr, &size});
  if (g_exc) return nullptr;
  call(tex_apply, tex);
  keep(tex);
  Rc r = {0, 0, static_cast<float>(N), static_cast<float>(N)};
  V2 pivot = {0.5f, 0.5f};
  void* spr = call(sprite_create, nullptr, {tex, &r, &pivot});
  return g_exc ? nullptr : keep(spr);
}

static void* new_go(const char* name) {
  void* go = A.object_new(k_go_c);
  call(go_ctor, go, {A.string_new(name)});
  return g_exc ? nullptr : keep(go);
}

static void set_rect(void* xf, float x0, float y0, float x1, float y1) {
  V2 a = {x0, y0}, b = {x1, y1}, z = {0, 0};
  call(rt_amin, xf, {&a});
  call(rt_amax, xf, {&b});
  call(rt_omin, xf, {&z});
  call(rt_omax, xf, {&z});
}

static void set_color_on(void* img, float r, float g, float b, float a) {
  Col c = {r, g, b, a};
  call(gr_set_color, img, {&c});
}

static bool glow_build() {
  if (g_glow_failed || !A.object_new) return false;
  const char* core = "UnityEngine.CoreModule";
  k_go_c = find_class(core, "UnityEngine", "GameObject");
  void* k_tex = find_class(core, "UnityEngine", "Texture2D");
  void* k_sprite = find_class(core, "UnityEngine", "Sprite");
  void* k_xf = find_class(core, "UnityEngine", "Transform");
  void* k_rt = find_class(core, "UnityEngine", "RectTransform");
  void* k_obj = find_class(core, "UnityEngine", "Object");
  void* k_canvas = find_class("UnityEngine.UIModule", "UnityEngine", "Canvas");
  void* k_image = find_class("UnityEngine.UI", "UnityEngine.UI", "Image");
  void* k_graphic = find_class("UnityEngine.UI", "UnityEngine.UI", "Graphic");

  go_ctor = find_method(k_go_c, ".ctor", 1, "System.String");
  go_addc = find_method(k_go_c, "AddComponent", 1, "System.Type");
  go_get_xf = find_method(k_go_c, "get_transform", 0);
  go_set_active = find_method(k_go_c, "SetActive", 1);
  xf_set_parent = find_method(k_xf, "SetParent", 2, "UnityEngine.Transform");
  dont_destroy = find_method(k_obj, "DontDestroyOnLoad", 1);
  rt_amin = find_method(k_rt, "set_anchorMin", 1);
  rt_amax = find_method(k_rt, "set_anchorMax", 1);
  rt_omin = find_method(k_rt, "set_offsetMin", 1);
  rt_omax = find_method(k_rt, "set_offsetMax", 1);
  cv_set_mode = find_method(k_canvas, "set_renderMode", 1);
  cv_set_order = find_method(k_canvas, "set_sortingOrder", 1);
  img_set_sprite = find_method(k_image, "set_sprite", 1);
  gr_set_color = find_method(k_graphic, "set_color", 1);
  gr_set_ray = find_method(k_graphic, "set_raycastTarget", 1);
  void* tex_ctor = find_tex_ctor(k_tex);
  void* tex_load = find_method(k_tex, "LoadRawTextureData", 2, "System.IntPtr");
  void* tex_apply = find_method(k_tex, "Apply", 0);
  void* sprite_create = find_method(k_sprite, "Create", 3, "UnityEngine.Texture2D");

  say(BEARITE_LOG_INFO,
      "glow: go=%d%d%d%d parent=%d dontdestroy=%d rect=%d%d%d%d canvas=%d%d image=%d%d%d tex=%d%d%d sprite=%d",
      go_ctor != nullptr, go_addc != nullptr, go_get_xf != nullptr, go_set_active != nullptr,
      xf_set_parent != nullptr, dont_destroy != nullptr, rt_amin != nullptr, rt_amax != nullptr,
      rt_omin != nullptr, rt_omax != nullptr, cv_set_mode != nullptr, cv_set_order != nullptr,
      img_set_sprite != nullptr, gr_set_color != nullptr, gr_set_ray != nullptr, tex_ctor != nullptr,
      tex_load != nullptr, tex_apply != nullptr, sprite_create != nullptr);

  if (!k_go_c || !k_tex || !k_canvas || !k_image || !go_ctor || !go_addc || !go_get_xf || !go_set_active ||
      !xf_set_parent || !rt_amin || !rt_amax || !rt_omin || !rt_omax || !cv_set_mode || !cv_set_order ||
      !img_set_sprite || !gr_set_color || !tex_ctor || !tex_load || !tex_apply || !sprite_create) {
    say(BEARITE_LOG_ERROR, "glow: something required was not found, glow is off");
    g_glow_failed = true;
    return false;
  }

  t_canvas = type_object(k_canvas);
  t_image = type_object(k_image);
  void* spr_glow = make_sprite(k_tex, tex_ctor, tex_load, tex_apply, sprite_create, 0);
  void* spr_vig = make_sprite(k_tex, tex_ctor, tex_load, tex_apply, sprite_create, 1);
  if (!spr_glow || !spr_vig) {
    say(BEARITE_LOG_ERROR, "glow: could not create the textures, glow is off");
    g_glow_failed = true;
    return false;
  }

  void* root = new_go("BearVisualsGlow");
  if (!root) { g_glow_failed = true; return false; }
  void* cv = call(go_addc, root, {t_canvas});
  if (g_exc || !cv) {
    say(BEARITE_LOG_ERROR, "glow: could not add a Canvas, glow is off");
    g_glow_failed = true;
    return false;
  }
  seti_o(cv_set_mode, cv, 0);      // ScreenSpaceOverlay
  seti_o(cv_set_order, cv, -100);  // below the game's HUD
  if (dont_destroy) call(dont_destroy, nullptr, {root});
  void* pxf = call(go_get_xf, root);

  auto layer = [&](const char* nm, void* spr, void*& img_out, void*& xf_out) -> bool {
    void* go = new_go(nm);
    if (!go) return false;
    void* img = call(go_addc, go, {t_image});
    if (g_exc || !img) return false;
    void* xf = call(go_get_xf, go);
    uint8_t keep_world = 0;
    call(xf_set_parent, xf, {pxf, &keep_world});
    call(img_set_sprite, img, {spr});
    if (gr_set_ray) call(gr_set_ray, img, {&keep_world});  // do not block touches
    img_out = keep(img);
    xf_out = keep(xf);
    return true;
  };
  if (!layer("Grade", nullptr, grade_img, grade_xf) || !layer("Vignette", spr_vig, vig_img, vig_xf) || !layer("Glow", spr_glow, glow_img, glow_xf)) {
    say(BEARITE_LOG_ERROR, "glow: could not create the image layers, glow is off");
    g_glow_failed = true;
    return false;
  }
  glow_root = root;
  g_glow_style = -1;
  say(BEARITE_LOG_INFO, "glow: overlay created");
  return true;
}

static void apply_glow(float pulse = 1.0f) {
  bool on = g_cfg.enabled && g_cfg.style > 0 && g_cfg.strength > 0.001f;
  if (!on) {
    if (glow_root) {
      setb_o(go_set_active, glow_root, false);
      if (g_exc) glow_root = nullptr;
    }
    return;
  }
  if (!glow_root && !glow_build()) return;
  setb_o(go_set_active, glow_root, true);
  if (g_exc) { glow_root = nullptr; return; }  // destroyed by a scene change, rebuild next time
  const GlowStyle& g = kGlow[g_cfg.style];
  if (g_glow_style != g_cfg.style) {
    set_rect(glow_xf, g.rect[0], g.rect[1], g.rect[2], g.rect[3]);
    set_rect(vig_xf, 0, 0, 1, 1);
    set_rect(grade_xf, 0, 0, 1, 1);
    g_glow_style = g_cfg.style;
  }
  float s = g_cfg.strength;
  set_color_on(glow_img, g.glow_col[0], g.glow_col[1], g.glow_col[2], std::min(1.0f, g.glow_a * s * pulse));
  set_color_on(grade_img, g.grade_col[0], g.grade_col[1], g.grade_col[2], std::min(1.0f, g.grade_a * s));
  set_color_on(vig_img, g.vig_col[0], g.vig_col[1], g.vig_col[2], std::min(1.0f, g.vig_a * s));
}

static void apply_config() {
  if (!g_cfg.enabled || g_cfg.style == 0) apply(nullptr, 0);
  else apply(&kStyles[g_cfg.style], g_cfg.strength);
  apply_shadows();
  apply_glow();
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
  g_t_glow += dt;
  g_time += dt;
  bool dirty = false;

  if (g_t_cfg >= 0.5f) {
    g_t_cfg = 0;
    Config c = read_config();
    if (!(c == g_cfg)) {
      say(BEARITE_LOG_INFO, "style: %s%s strength=%.2f shadows=%d", kStyles[c.style].name,
          c.enabled ? "" : " (off)", c.strength, c.shadows);
      g_cfg = c;
      dirty = true;
    }
  }

  if (g_t_scan >= 3.0f) {
    g_t_scan = 0;
    bool first = g_known.empty();
    if (scan_materials()) dirty = true;
    if (first && !g_known.empty()) log_shaders();
    if (scan_renderers()) dirty = true;
    refresh_sun();
    if (!dirty) {
      // the game may reset fog / shadows by itself
      if (g_touched) apply_fog(&kStyles[g_cfg.style], g_cfg.strength);
      apply_shadows();
    }
  }

  if (dirty) {
    apply_config();
    g_t_glow = 0;
  } else if (g_t_glow >= 0.25f) {
    g_t_glow = 0;
    // slow breathing of the glow; also rebuilds the overlay if a scene change destroyed it
    if (g_cfg.enabled && g_cfg.style > 0) apply_glow(1.0f + 0.07f * std::sin(g_time * 1.2f));
  }
}

static void unload() {
  if (!g_ready) return;
  if (g_touched) apply(nullptr, 0);
  g_cfg.shadows = false;
  apply_shadows();
  g_cfg.style = 0;
  apply_glow();
}

}  // namespace vis

// ============================================================ 4. exports

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  if (api && api->mod_dir) vis::g_dir = api->mod_dir;
  say(BEARITE_LOG_INFO, "Visual Styles 0.7 loaded, mod dir: %s", vis::g_dir.c_str());
  return 0;
}

BEARITE_EXPORT void bearite_on_update(float dt) { vis::update(dt); }

BEARITE_EXPORT void bearite_on_unload(void) { vis::unload(); }

}  // extern "C"
