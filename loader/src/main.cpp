// Bearite loader entry point. Runs when the game calls System.loadLibrary("bearite").
#include <dlfcn.h>
#include <jni.h>

#include <algorithm>
#include <deque>
#include <set>

#include "abi.h"
#include "guard.hpp"
#include "log.hpp"
#include "mods.hpp"
#include "runtime.hpp"
#include "settings.hpp"

namespace {

using namespace bearite;

constexpr const char* kTag = "loader";
constexpr const char* kLoaderVersion = "0.2.0";

// One loaded mod. Lives for the whole process: the mod keeps a pointer to `api`.
struct Loaded {
  Mod* mod;
  BeariteApi api{};
  BeariteOnUpdateFn on_update = nullptr;
  BeariteOnUnloadFn on_unload = nullptr;
  Settings settings;
  bool active = true;
  explicit Loaded(Mod* m) : mod(m), settings(m->dir / "settings.json") {}
};

std::vector<Mod> g_mods;
std::deque<Loaded> g_loaded;
std::string g_game_version;

// ---------------------------------------------------------------- JNI helpers

struct AppInfo {
  std::string package, version, files_dir, external_dir;
};

bool exception_pending(JNIEnv* env) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear();
  return true;
}

std::string to_string(JNIEnv* env, jstring s) {
  if (!s) return "";
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

std::string file_path(JNIEnv* env, jobject file) {
  if (!file) return "";
  jclass cls = env->FindClass("java/io/File");
  jmethodID m = env->GetMethodID(cls, "getAbsolutePath", "()Ljava/lang/String;");
  jstring s = static_cast<jstring>(env->CallObjectMethod(file, m));
  if (exception_pending(env)) return "";
  return to_string(env, s);
}

bool query_app_info(JNIEnv* env, AppInfo& info) {
  if (env->PushLocalFrame(64) != 0) return false;
  do {
    jclass thread = env->FindClass("android/app/ActivityThread");
    if (!thread) break;
    jmethodID current = env->GetStaticMethodID(thread, "currentApplication",
                                               "()Landroid/app/Application;");
    if (!current) break;
    jobject app = env->CallStaticObjectMethod(thread, current);
    if (exception_pending(env) || !app) break;
    jclass ctx = env->GetObjectClass(app);

    jmethodID get_name = env->GetMethodID(ctx, "getPackageName", "()Ljava/lang/String;");
    jstring jname = static_cast<jstring>(env->CallObjectMethod(app, get_name));
    if (exception_pending(env) || !jname) break;
    info.package = to_string(env, jname);

    jmethodID get_files = env->GetMethodID(ctx, "getFilesDir", "()Ljava/io/File;");
    jobject files = env->CallObjectMethod(app, get_files);
    exception_pending(env);
    info.files_dir = file_path(env, files);

    jmethodID get_ext =
        env->GetMethodID(ctx, "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;");
    jobject ext = env->CallObjectMethod(app, get_ext, static_cast<jstring>(nullptr));
    exception_pending(env);
    info.external_dir = file_path(env, ext);

    jmethodID get_pm =
        env->GetMethodID(ctx, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    jobject pm = env->CallObjectMethod(app, get_pm);
    if (exception_pending(env) || !pm) break;
    jclass pm_cls = env->GetObjectClass(pm);
    jmethodID get_info = env->GetMethodID(
        pm_cls, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    jobject pi = env->CallObjectMethod(pm, get_info, jname, 0);
    if (!exception_pending(env) && pi) {
      jclass pi_cls = env->GetObjectClass(pi);
      jfieldID vn = env->GetFieldID(pi_cls, "versionName", "Ljava/lang/String;");
      info.version = to_string(env, static_cast<jstring>(env->GetObjectField(pi, vn)));
    }
  } while (false);
  exception_pending(env);
  env->PopLocalFrame(nullptr);
  return !info.files_dir.empty();
}

// ------------------------------------------------------------- API for mods

Loaded* self(const BeariteApi* api) {
  return api ? static_cast<Loaded*>(api->internal) : nullptr;
}

void api_log(int level, const char* tag, const char* msg) {
  Logger::get().write(level, tag ? tag : "mod", msg ? msg : "");
}

int api_hook_method(const BeariteApi* api, const char* assembly, const char* ns,
                    const char* klass, const char* method, int argc, void* replacement,
                    void** out_original) {
  Loaded* l = self(api);
  if (!l) return 0;
  return Runtime::get().add_hook(l->mod->id, assembly, ns, klass, method, argc, replacement,
                                 out_original);
}

void* api_find_method(const BeariteApi*, const char* assembly, const char* ns,
                      const char* klass, const char* method, int argc) {
  return Runtime::get().find_method(assembly, ns, klass, method, argc);
}

int api_il2cpp_ready(const BeariteApi*) { return Runtime::get().ready() ? 1 : 0; }

int api_get_setting(const BeariteApi* api, const char* key, const char* def, char* out,
                    uint32_t out_size) {
  Loaded* l = self(api);
  if (!l || !key || !out || out_size == 0) return 0;
  std::string value;
  bool found = l->settings.get(key, value);
  if (!found) value = def ? def : "";
  size_t n = std::min<size_t>(value.size(), out_size - 1);
  memcpy(out, value.data(), n);
  out[n] = '\0';
  return found ? 1 : 0;
}

int api_set_setting(const BeariteApi* api, const char* key, const char* value) {
  Loaded* l = self(api);
  if (!l || !key) return 0;
  return l->settings.set(key, value ? value : "") ? 1 : 0;
}

void fill_api(Loaded& l) {
  BeariteApi& a = l.api;
  a.abi_version = BEARITE_ABI_VERSION;
  a.struct_size = sizeof(BeariteApi);
  a.game_version = g_game_version.c_str();
  a.mod_dir = l.mod->dir.c_str();
  a.log = api_log;
  a.internal = &l;
  a.hook_method = api_hook_method;
  a.find_method = api_find_method;
  a.il2cpp_ready = api_il2cpp_ready;
  a.get_setting = api_get_setting;
  a.set_setting = api_set_setting;
}

// ------------------------------------------------- guarded calls into mod code

struct LoadCall {
  BeariteOnLoadFn fn;
  const BeariteApi* api;
  int result;
};
void do_on_load(void* p) {
  auto* c = static_cast<LoadCall*>(p);
  c->result = c->fn(c->api);
}

struct UpdateCall {
  BeariteOnUpdateFn fn;
  float dt;
};
void do_on_update(void* p) {
  auto* c = static_cast<UpdateCall*>(p);
  c->fn(c->dt);
}

struct UnloadCall {
  BeariteOnUnloadFn fn;
};
void do_on_unload(void* p) { static_cast<UnloadCall*>(p)->fn(); }

std::string describe(int r) {
  return r < 0 ? "threw a C++ exception" : "crashed (signal " + std::to_string(r) + ")";
}

// Turns a mod off after a runtime error: removes its hooks, calls on_unload.
void disable_mod(Loaded& l, const std::string& why) {
  if (!l.active) return;
  l.active = false;
  l.mod->enabled = false;
  l.mod->reason = why;
  BLOG_E(kTag, "disabling %s: %s", l.mod->id.c_str(), why.c_str());
  Runtime::get().remove_hooks(l.mod->id);
  if (l.on_unload) {
    UnloadCall call{l.on_unload};
    guarded_call(do_on_unload, &call);
  }
}

// Called every frame by the runtime.
void dispatch_update(float dt) {
  guard_detail::Scope scope;
  for (Loaded& l : g_loaded) {
    if (!l.active || !l.on_update) continue;
    UpdateCall call{l.on_update, dt};
    int r = guarded_call(do_on_update, &call);
    if (r != 0) disable_mod(l, "on_update " + describe(r));
  }
}

// ------------------------------------------------------------------- loading

bool make_dirs(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  return fs::is_directory(p, ec);
}

bool load_mod(Mod& mod, const fs::path& cache_dir, std::string& error) {
  std::error_code ec;
  fs::path src = mod.dir / mod.library;
  if (!fs::is_regular_file(src, ec)) {
    error = "library not found: " + mod.library;
    return false;
  }
  // Executable code cannot be loaded from shared storage, so copy it first.
  fs::path dst_dir = cache_dir / mod.id;
  fs::remove_all(dst_dir, ec);
  fs::create_directories(dst_dir, ec);
  fs::path dst = dst_dir / mod.library;
  fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
  if (ec) {
    error = "cannot copy library: " + ec.message();
    return false;
  }
  fs::permissions(dst, fs::perms::owner_read | fs::perms::owner_exec, ec);

  void* handle = dlopen(dst.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* why = dlerror();
    error = std::string("dlopen failed: ") + (why ? why : "unknown");
    return false;
  }
  auto on_load = reinterpret_cast<BeariteOnLoadFn>(dlsym(handle, "bearite_on_load"));
  if (!on_load) {
    error = "library does not export bearite_on_load";
    dlclose(handle);
    return false;
  }

  g_loaded.emplace_back(&mod);
  Loaded& l = g_loaded.back();
  l.on_update = reinterpret_cast<BeariteOnUpdateFn>(dlsym(handle, "bearite_on_update"));
  l.on_unload = reinterpret_cast<BeariteOnUnloadFn>(dlsym(handle, "bearite_on_unload"));
  fill_api(l);

  LoadCall call{on_load, &l.api, 0};
  int r = guarded_call(do_on_load, &call);
  if (r != 0 || call.result != 0) {
    error = r != 0 ? "bearite_on_load " + describe(r)
                   : "bearite_on_load returned " + std::to_string(call.result);
    l.active = false;
    Runtime::get().remove_hooks(mod.id);  // drop hooks it queued before failing
    return false;
  }
  return true;
}

void start(JavaVM* vm) {
  JNIEnv* env = nullptr;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK || !env) return;

  AppInfo app;
  if (!query_app_info(env, app)) {
    __android_log_print(ANDROID_LOG_ERROR, "Bearite", "cannot read app info, loader stopped");
    return;
  }
  g_game_version = app.version;

  // Prefer the app's shared folder (easy to reach), fall back to private storage.
  fs::path private_root = fs::path(app.files_dir) / "bearite";
  fs::path root = private_root;
  if (!app.external_dir.empty()) {
    fs::path ext = fs::path(app.external_dir) / "bearite";
    if (make_dirs(ext / "mods") && make_dirs(ext / "logs")) root = ext;
  }
  if (root == private_root) {
    make_dirs(root / "mods");
    make_dirs(root / "logs");
  }
  fs::path mods_dir = root / "mods";
  fs::path log_path = root / "logs" / "bearite.log";
  fs::path cache_dir = private_root / "loaded";
  make_dirs(cache_dir);

  std::error_code ec;
  fs::rename(log_path, root / "logs" / "bearite.old.log", ec);
  Logger::get().open_file(log_path.string());

  BLOG_I(kTag, "Bearite loader %s, ABI %d", kLoaderVersion, BEARITE_ABI_VERSION);
  BLOG_I(kTag, "package=%s game_version=%s", app.package.c_str(),
         g_game_version.empty() ? "(unknown)" : g_game_version.c_str());
  BLOG_I(kTag, "mods folder: %s", mods_dir.c_str());
  if (g_game_version.empty()) BLOG_W(kTag, "game version unknown, version checks skipped");

  // 1. Find and parse mods.
  for (fs::directory_iterator it(mods_dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    Mod mod;
    std::string error;
    if (!parse_mod(it->path(), mod, error)) {
      BLOG_W(kTag, "skipped '%s': %s", it->path().filename().c_str(), error.c_str());
      continue;
    }
    g_mods.push_back(std::move(mod));
  }
  std::sort(g_mods.begin(), g_mods.end(),
            [](const Mod& a, const Mod& b) { return a.id < b.id; });

  // 2. Pre-checks: duplicate ids and game version.
  for (size_t i = 0; i < g_mods.size(); ++i) {
    Mod& m = g_mods[i];
    if (i > 0 && g_mods[i - 1].id == m.id) {
      m.enabled = false;
      m.reason = "duplicate id";
    } else if (!supports_game_version(m, g_game_version)) {
      m.enabled = false;
      m.reason = "game version " + g_game_version + " is not supported by this mod";
    }
  }

  // 3. Dependency order, then load.
  std::vector<Mod*> order = resolve_load_order(g_mods);
  std::set<std::string> loaded;
  for (Mod* m : order) {
    std::string error;
    for (const Dependency& dep : m->deps) {
      if (!dep.optional && !loaded.count(dep.id)) {
        error = "dependency '" + dep.id + "' failed to load";
        break;
      }
    }
    if (error.empty() && load_mod(*m, cache_dir, error)) {
      loaded.insert(m->id);
      BLOG_I(kTag, "loaded: %s %s by %s", m->id.c_str(), m->version.c_str(),
             m->author.empty() ? "?" : m->author.c_str());
    } else {
      m->enabled = false;
      m->reason = error;
    }
  }
  for (const Mod& m : g_mods)
    if (!m.enabled) BLOG_W(kTag, "disabled: %s: %s", m.id.c_str(), m.reason.c_str());
  BLOG_I(kTag, "done: %zu of %zu mods loaded", loaded.size(), g_mods.size());

  // 4. Hooks and on_update need the game's IL2CPP runtime: wait for it in the background.
  if (!loaded.empty()) {
    Runtime::get().set_update_callback(dispatch_update);
    Runtime::get().start_watcher();
  }
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
  try {
    start(vm);
  } catch (...) {
    __android_log_print(ANDROID_LOG_ERROR, "Bearite", "loader failed with an exception");
  }
  return JNI_VERSION_1_6;  // never stop the game because of the loader
}
