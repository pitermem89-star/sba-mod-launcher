// noads.cpp: "No Ads" mod for Super Bear Adventure (v1.0), a separate Bearite mod.
//
// The game shows ads in exactly two places (found by reading its code):
//  1. full-screen ads between levels: AdsManager.TryDisplayInterstitial asks
//     AdsManager.IsInterstitialReady() first. Answering "no" sends the game down its normal
//     "no ad available" path, so nothing is shown and nothing gets stuck.
//  2. ads you start for a reward (revive, double coins...): AdsManager.ShowRewardedAd checks the
//     rules, remembers the reward and later calls AdsManager.ShowRewardedAdSdk, which opens the
//     ad. The mod lets all of that run, but where the ad would open it calls the game's own
//     "ad was watched" function (AdsManager.OnRewardedAdComplete) instead. IsRewardedReady()
//     answers "yes", so the button never waits for an ad to load.
// Purchases and the premium checks are not touched.
//
// Settings (mod menu): no_ads, skip_rewarded. Both on by default.
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
  if (bearite::api()) bearite::log(level, "noads", "%s", buf);
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





}  // namespace il

namespace noads {

static std::string g_dir;

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

static bool g_no_ads = true;         // setting "no_ads"
static bool g_skip_rewarded = true;  // setting "skip_rewarded"
static float g_t_noads = 10;
static float g_ad_clock = 0;
static int g_ads_blocked = 0;

static bool (*o_int_ready)(const void*) = nullptr;
static bool (*o_rew_ready)(const void*) = nullptr;
static void (*o_show_rewarded)(void*, int, const void*) = nullptr;
static void (*o_show_rewarded_sdk)(void*, const void*) = nullptr;

static void* g_ad_self = nullptr;  // the AdsManager that is waiting to "finish" a rewarded ad
static float g_ad_self_time = 0;
static void* m_ad_complete = nullptr;
static bool g_ad_resolve_failed = false;

static void note_blocked(const char* what) {
  ++g_ads_blocked;
  if (g_ads_blocked <= 3 || g_ads_blocked % 200 == 0)
    say(BEARITE_LOG_INFO, "%s (%d times)", what, g_ads_blocked);
}

// Static methods get only the MethodInfo* argument.
static bool hk_int_ready(const void* mi) {
  if (g_no_ads) { note_blocked("IsInterstitialReady -> not ready"); return false; }
  return o_int_ready ? o_int_ready(mi) : false;
}

static bool hk_rew_ready(const void* mi) {
  if (g_skip_rewarded) return true;
  return o_rew_ready ? o_rew_ready(mi) : false;
}

// Instance methods get `this` first. The game itself passes a null MethodInfo to these.
static void hk_show_rewarded(void* self, int reward, const void* mi) {
  if (g_skip_rewarded && self) {
    g_ad_self = self;
    g_ad_self_time = g_ad_clock;
  }
  if (o_show_rewarded) o_show_rewarded(self, reward, mi);
}

// `this` is NOT reliable here: when the game opens the ad straight away it does not pass it
// (the original does not use it), so the instance saved in hk_show_rewarded is used.
static void hk_show_rewarded_sdk(void* self, const void* mi) {
  if (g_skip_rewarded && g_ad_self && g_ad_clock - g_ad_self_time < 20.0f && !g_ad_resolve_failed) {
    if (!m_ad_complete && il::init()) {
      void* klass = il::find_class("Assembly-CSharp", "", "AdsManager");
      m_ad_complete = il::find_method(klass, "OnRewardedAdComplete", 0);
      if (!m_ad_complete) {
        g_ad_resolve_failed = true;
        say(BEARITE_LOG_WARN, "AdsManager.OnRewardedAdComplete not found, rewarded ads stay as they are");
      }
    }
    if (m_ad_complete) {
      void* inst = g_ad_self;
      g_ad_self = nullptr;  // one reward per request
      il::call(m_ad_complete, inst);
      if (!il::g_exc) {
        note_blocked("rewarded ad skipped, reward given");
        return;
      }
      say(BEARITE_LOG_WARN, "OnRewardedAdComplete threw an exception, showing the real ad");
    }
  }
  if (o_show_rewarded_sdk) o_show_rewarded_sdk(self, mi);
}

static void install_no_ads() {
  const char* game = "Assembly-CSharp";
  bool a = bearite::hook(game, "", "AdsManager", "IsInterstitialReady", 0,
                         reinterpret_cast<void*>(&hk_int_ready), reinterpret_cast<void**>(&o_int_ready));
  bool b = bearite::hook(game, "", "AdsManager", "IsRewardedReady", 0,
                         reinterpret_cast<void*>(&hk_rew_ready), reinterpret_cast<void**>(&o_rew_ready));
  bool c = bearite::hook(game, "", "AdsManager", "ShowRewardedAd", 1,
                         reinterpret_cast<void*>(&hk_show_rewarded), reinterpret_cast<void**>(&o_show_rewarded));
  bool d = bearite::hook(game, "", "AdsManager", "ShowRewardedAdSdk", 0,
                         reinterpret_cast<void*>(&hk_show_rewarded_sdk),
                         reinterpret_cast<void**>(&o_show_rewarded_sdk));
  say(BEARITE_LOG_INFO,
      "hooks queued IsInterstitialReady=%d IsRewardedReady=%d ShowRewardedAd=%d ShowRewardedAdSdk=%d",
      a, b, c, d);
}

static void update(float dt) {
  g_ad_clock += dt;
  g_t_noads += dt;
  if (g_t_noads < 1.0f) return;
  g_t_noads = 0;
  std::string file = read_file(g_dir + "/settings.json");
  double v;
  bool a = true, b = true;
  if (get_setting_num(file, "no_ads", v)) a = v != 0;
  if (get_setting_num(file, "skip_rewarded", v)) b = v != 0;
  if (a != g_no_ads) say(BEARITE_LOG_INFO, "between-level ads %s", a ? "blocked" : "allowed");
  if (b != g_skip_rewarded) say(BEARITE_LOG_INFO, "rewarded ads %s", b ? "skipped" : "shown");
  g_no_ads = a;
  g_skip_rewarded = b;
}

static void unload() {  // the loader removes the hooks; until then they just pass through
  g_no_ads = false;
  g_skip_rewarded = false;
}

}  // namespace noads

extern "C" {

BEARITE_EXPORT int bearite_on_load(const BeariteApi* api) {
  bearite::init(api);
  if (api && api->mod_dir) noads::g_dir = api->mod_dir;
  say(BEARITE_LOG_INFO, "No Ads 1.0 loaded, mod dir: %s", noads::g_dir.c_str());
  noads::install_no_ads();
  return 0;
}

BEARITE_EXPORT void bearite_on_update(float dt) { noads::update(dt); }

BEARITE_EXPORT void bearite_on_unload(void) { noads::unload(); }

}  // extern "C"
