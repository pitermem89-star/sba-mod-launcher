// IL2CPP access, hook installation and the per-frame tick. Header-only.
#pragma once

#include <dlfcn.h>
#include <time.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "dobby.h"
#include "guard.hpp"
#include "log.hpp"

namespace bearite {

// Exported functions of libil2cpp.so that we use.
struct Il2CppApi {
  void* (*domain_get)() = nullptr;
  void* (*domain_assembly_open)(void*, const char*) = nullptr;
  void* (*assembly_get_image)(void*) = nullptr;
  void* (*class_from_name)(void*, const char*, const char*) = nullptr;
  void* (*class_get_method_from_name)(void*, const char*, int) = nullptr;
  void* (*thread_attach)(void*) = nullptr;
  void* (*thread_get_all)(size_t*) = nullptr;  // optional
};

class Runtime {
 public:
  static Runtime& get() {
    static Runtime r;
    return r;
  }

  bool ready() const { return ready_.load(); }

  void set_update_callback(std::function<void(float)> cb) { update_cb_ = std::move(cb); }

  // Queues a hook (installed at once if IL2CPP is already ready).
  int add_hook(const std::string& owner, const char* assembly, const char* ns,
               const char* klass, const char* method, int argc, void* replacement,
               void** out_original) {
    if (!assembly || !klass || !method || !replacement || argc < 0) return 0;
    if (std::string(klass) == "EventSystem" && std::string(method) == "Update") {
      BLOG_W("runtime", "[%s] EventSystem.Update is reserved for on_update", owner.c_str());
      return 0;
    }
    Hook h;
    h.owner = owner;
    h.assembly = assembly;
    h.ns = ns ? ns : "";
    h.klass = klass;
    h.method = method;
    h.argc = argc;
    h.replacement = replacement;
    h.out_original = out_original;
    std::lock_guard<std::mutex> lock(mutex_);
    hooks_.push_back(h);
    if (ready_.load()) install(hooks_.back());
    return 1;
  }

  void* find_method(const char* assembly, const char* ns, const char* klass,
                    const char* method, int argc) {
    if (!ready_.load() || !assembly || !klass || !method) return nullptr;
    return resolve(assembly, ns ? ns : "", klass, method, argc);
  }

  // Removes all hooks that belong to one mod.
  void remove_hooks(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = hooks_.begin(); it != hooks_.end();) {
      if (it->owner == owner) {
        if (it->installed && it->target) DobbyDestroy(it->target);
        it = hooks_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // Starts a background thread that waits for the game's IL2CPP runtime.
  void start_watcher() {
    std::thread([this] { watch(); }).detach();
  }

 private:
  struct Hook {
    std::string owner, assembly, ns, klass, method;
    int argc = 0;
    void* replacement = nullptr;
    void** out_original = nullptr;
    void* target = nullptr;
    bool done = false;       // tried once (success or failure)
    bool installed = false;  // hook is active
  };

  struct ResolveCall {
    const Il2CppApi* api;
    void* domain;
    std::string assembly, ns, klass, method;
    int argc;
    void* result;
  };
  static void do_resolve(void* p) {
    auto* c = static_cast<ResolveCall*>(p);
    void* assembly = c->api->domain_assembly_open(c->domain, c->assembly.c_str());
    if (!assembly) return;
    void* image = c->api->assembly_get_image(assembly);
    if (!image) return;
    void* cls = c->api->class_from_name(image, c->ns.c_str(), c->klass.c_str());
    if (!cls) return;
    void* info = c->api->class_get_method_from_name(cls, c->method.c_str(), c->argc);
    if (info) c->result = *static_cast<void**>(info);  // MethodInfo starts with methodPointer
  }

  // Native address of a method, or nullptr.
  void* resolve(std::string assembly, const std::string& ns, const std::string& klass,
                const std::string& method, int argc) {
    if (assembly.size() > 4 && assembly.compare(assembly.size() - 4, 4, ".dll") == 0)
      assembly.resize(assembly.size() - 4);
    BLOG_I("runtime", "resolving %s::%s", klass.c_str(), method.c_str());
    ResolveCall call{&api_, domain_, assembly, ns, klass, method, argc, nullptr};
    int r = guarded_call(do_resolve, &call);
    if (r != 0) {
      BLOG_E("runtime", "resolve of %s::%s failed (%d)", klass.c_str(), method.c_str(), r);
      return nullptr;
    }
    return call.result;
  }

  void install(Hook& h) {  // mutex_ must be held
    h.done = true;
    void* target = resolve(h.assembly, h.ns, h.klass, h.method, h.argc);
    if (!target) {
      BLOG_W("runtime", "[%s] method not found: %s.%s::%s (%d args)", h.owner.c_str(),
             h.ns.c_str(), h.klass.c_str(), h.method.c_str(), h.argc);
      return;
    }
    BLOG_I("runtime", "installing hook on %s::%s", h.klass.c_str(), h.method.c_str());
    void* dummy = nullptr;
    int rc = DobbyHook(target, h.replacement, h.out_original ? h.out_original : &dummy);
    if (rc != 0) {
      BLOG_E("runtime", "[%s] DobbyHook failed (%d) for %s::%s", h.owner.c_str(), rc,
             h.klass.c_str(), h.method.c_str());
      return;
    }
    h.target = target;
    h.installed = true;
    BLOG_I("runtime", "[%s] hooked %s.%s::%s", h.owner.c_str(), h.ns.c_str(), h.klass.c_str(),
           h.method.c_str());
  }

  bool bind(void* lib) {
    api_.domain_get = reinterpret_cast<decltype(api_.domain_get)>(dlsym(lib, "il2cpp_domain_get"));
    api_.domain_assembly_open = reinterpret_cast<decltype(api_.domain_assembly_open)>(
        dlsym(lib, "il2cpp_domain_assembly_open"));
    api_.assembly_get_image = reinterpret_cast<decltype(api_.assembly_get_image)>(
        dlsym(lib, "il2cpp_assembly_get_image"));
    api_.class_from_name =
        reinterpret_cast<decltype(api_.class_from_name)>(dlsym(lib, "il2cpp_class_from_name"));
    api_.class_get_method_from_name = reinterpret_cast<decltype(api_.class_get_method_from_name)>(
        dlsym(lib, "il2cpp_class_get_method_from_name"));
    api_.thread_attach =
        reinterpret_cast<decltype(api_.thread_attach)>(dlsym(lib, "il2cpp_thread_attach"));
    api_.thread_get_all = reinterpret_cast<decltype(api_.thread_get_all)>(
        dlsym(lib, "il2cpp_thread_get_all_attached_threads"));
    bool ok = api_.domain_get && api_.domain_assembly_open && api_.assembly_get_image &&
              api_.class_from_name && api_.class_get_method_from_name && api_.thread_attach;
    if (!ok) BLOG_E("runtime", "libil2cpp.so does not export the il2cpp_* functions we need");
    if (!api_.thread_get_all) BLOG_W("runtime", "no thread list API, using a fixed delay");
    return ok;
  }

  static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

  void watch() {
    // 1. Wait until Unity has loaded libil2cpp.so (up to 2 minutes).
    void* lib = nullptr;
    for (int i = 0; i < 1200 && !lib; ++i) {
      lib = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
      if (!lib) sleep_ms(100);
    }
    if (!lib) {
      BLOG_E("runtime", "libil2cpp.so never appeared, hooks disabled");
      return;
    }
    BLOG_I("runtime", "libil2cpp.so found, waiting for the runtime");
    if (!bind(lib)) return;

    // 2. Wait until the game's main thread is attached to IL2CPP, which means Unity has
    //    finished starting the scripting runtime. No unsafe jumps here.
    BLOG_I("runtime", "step 1/4: waiting for the scripting runtime to start");
    bool started = false;
    for (int i = 0; i < 600 && !started; ++i) {
      sleep_ms(200);
      if (i < 25) continue;  // never look before 5 seconds have passed
      if (api_.thread_get_all) {
        size_t n = 0;
        api_.thread_get_all(&n);
        started = n > 0;
      } else {
        started = i >= 75;  // no API: wait 15 seconds
      }
    }
    if (!started) {
      BLOG_E("runtime", "scripting runtime did not start, hooks disabled");
      return;
    }
    BLOG_I("runtime", "step 2/4: runtime started, short grace period");
    sleep_ms(3000);

    domain_ = api_.domain_get();
    if (!domain_) {
      BLOG_E("runtime", "no IL2CPP domain, hooks disabled");
      return;
    }

    // 3. Attach this thread to IL2CPP before using any other API function.
    BLOG_I("runtime", "step 3/4: attaching thread");
    api_.thread_attach(domain_);

    // 4. Wait until the game code is available.
    BLOG_I("runtime", "step 4/4: opening Assembly-CSharp");
    void* assembly = nullptr;
    for (int i = 0; i < 60 && !assembly; ++i) {
      assembly = api_.domain_assembly_open(domain_, "Assembly-CSharp");
      if (!assembly) sleep_ms(1000);
    }
    if (!assembly) {
      BLOG_E("runtime", "Assembly-CSharp not found, hooks disabled");
      return;
    }
    BLOG_I("runtime", "IL2CPP runtime is ready");

    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = true;
    for (Hook& h : hooks_)
      if (!h.done) install(h);
    install_tick();
  }

  // ---- per-frame tick: EventSystem.Update runs once per frame in games with UI ----
  using TickFn = void (*)(void*, const void*);
  inline static TickFn tick_original_ = nullptr;

  static void tick_hook(void* self, const void* method) {
    if (tick_original_) tick_original_(self, method);
    get().on_tick();
  }

  void on_tick() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
    double dt = last_tick_ > 0 ? now - last_tick_ : 0.0;
    if (last_tick_ > 0 && dt < 0.002) return;  // several EventSystems in one frame
    last_tick_ = now;
    if (update_cb_) update_cb_(static_cast<float>(dt));
  }

  void install_tick() {  // mutex_ must be held
    void* target = resolve("UnityEngine.UI", "UnityEngine.EventSystems", "EventSystem", "Update", 0);
    if (!target) {
      BLOG_W("runtime", "on_update disabled: EventSystem.Update not found");
      return;
    }
    BLOG_I("runtime", "installing tick hook");
    int rc = DobbyHook(target, reinterpret_cast<void*>(&Runtime::tick_hook),
                       reinterpret_cast<void**>(&tick_original_));
    if (rc != 0) {
      BLOG_E("runtime", "on_update disabled: DobbyHook failed (%d)", rc);
      return;
    }
    BLOG_I("runtime", "on_update is driven by EventSystem.Update");
  }

  std::mutex mutex_;
  std::vector<Hook> hooks_;
  std::atomic<bool> ready_{false};
  Il2CppApi api_;
  void* domain_ = nullptr;
  std::function<void(float)> update_cb_;
  double last_tick_ = 0;
};

}  // namespace bearite
