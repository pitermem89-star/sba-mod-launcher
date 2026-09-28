// Bearite loader entry point. Runs when the game calls System.loadLibrary("bearite").
#include <dlfcn.h>
#include <jni.h>
#include <setjmp.h>
#include <signal.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <deque>
#include <set>

#include "abi.h"
#include "log.hpp"
#include "mods.hpp"

namespace {

using namespace bearite;

constexpr const char* kTag = "loader";
constexpr const char* kLoaderVersion = "0.1.0";

// Kept alive for the whole process: mods may keep pointers into them.
std::vector<Mod> g_mods;
std::deque<BeariteApi> g_apis;
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

// java.io.File -> absolute path ("" if null)
std::string file_path(JNIEnv* env, jobject file) {
  if (!file) return "";
  jclass cls = env->FindClass("java/io/File");
  jmethodID m = env->GetMethodID(cls, "getAbsolutePath", "()Ljava/lang/String;");
  jstring s = static_cast<jstring>(env->CallObjectMethod(file, m));
  if (exception_pending(env)) return "";
  return to_string(env, s);
}

// Reads package name, game versionName and app folders from the running app.
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

// --------------------------------------------------------------- crash guard
// Runs a mod's bearite_on_load so that a crash inside it disables only that
// mod. Only protects the calling thread and only during this call.

constexpr int kSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE};
constexpr int kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);

sigjmp_buf* g_jump = nullptr;
long g_guard_tid = 0;

void crash_handler(int sig) {
  if (g_jump && syscall(SYS_gettid) == g_guard_tid) siglongjmp(*g_jump, sig);
  signal(sig, SIG_DFL);  // crash on another thread: behave as without guard
  raise(sig);
}

int run_on_load(BeariteOnLoadFn fn, const BeariteApi* api, int* result) {
  try {
    *result = fn(api);
    return 0;
  } catch (...) {
    return -1;
  }
}

// Returns 0 if the call finished, -1 on C++ exception, >0 = signal number.
int call_guarded(BeariteOnLoadFn fn, const BeariteApi* api, int* result) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = crash_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_NODEFER;
  struct sigaction old[kSignalCount];
  for (int i = 0; i < kSignalCount; ++i) sigaction(kSignals[i], &sa, &old[i]);

  sigjmp_buf jump;
  g_jump = &jump;
  g_guard_tid = syscall(SYS_gettid);
  int sig = sigsetjmp(jump, 1);
  if (sig == 0) sig = run_on_load(fn, api, result);
  g_jump = nullptr;

  for (int i = 0; i < kSignalCount; ++i) sigaction(kSignals[i], &old[i], nullptr);
  return sig;
}

// ------------------------------------------------------------------- loading

void api_log(int level, const char* tag, const char* msg) {
  Logger::get().write(level, tag ? tag : "mod", msg ? msg : "");
}

const BeariteApi* make_api(const Mod& mod) {
  BeariteApi api{};
  api.abi_version = BEARITE_ABI_VERSION;
  api.struct_size = sizeof(BeariteApi);
  api.game_version = g_game_version.c_str();
  api.mod_dir = mod.dir.c_str();
  api.log = api_log;
  g_apis.push_back(api);
  return &g_apis.back();
}

bool make_dirs(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  return fs::is_directory(p, ec);
}

// Copies the mod library to the app's private folder, dlopens it and calls
// bearite_on_load. Returns false and sets `error` on any problem.
bool load_mod(const Mod& mod, const fs::path& cache_dir, std::string& error) {
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

  int result = 0;
  int sig = call_guarded(on_load, make_api(mod), &result);
  if (sig < 0) {
    error = "bearite_on_load threw a C++ exception";
    return false;
  }
  if (sig > 0) {
    error = "bearite_on_load crashed (signal " + std::to_string(sig) + ")";
    return false;  // not dlclosed on purpose: the library is in an unknown state
  }
  if (result != 0) {
    error = "bearite_on_load returned " + std::to_string(result);
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
