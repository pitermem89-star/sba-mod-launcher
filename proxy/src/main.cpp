// Proxy for the game's libmain.so: starts Bearite, then hands over to the real libmain.
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#define TAG "bearite"

typedef jint (*OnLoadFn)(JavaVM*, void*);

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  // 1. Start Bearite. A plain dlopen does not call JNI_OnLoad, so we call it ourselves.
  void* mod = dlopen("libbearite.so", RTLD_NOW | RTLD_LOCAL);
  if (!mod) {
    __android_log_print(ANDROID_LOG_ERROR, TAG, "libbearite.so: %s", dlerror());
  } else {
    OnLoadFn start = reinterpret_cast<OnLoadFn>(dlsym(mod, "JNI_OnLoad"));
    if (start) {
      start(vm, reserved);
    } else {
      __android_log_print(ANDROID_LOG_ERROR, TAG, "JNI_OnLoad missing in libbearite.so");
    }
  }

  // 2. Hand over to the original libmain (renamed by the patcher).
  void* real = dlopen("libmain_real.so", RTLD_NOW);
  if (!real) {
    __android_log_print(ANDROID_LOG_ERROR, TAG, "libmain_real.so: %s", dlerror());
    return JNI_ERR;
  }
  OnLoadFn fn = reinterpret_cast<OnLoadFn>(dlsym(real, "JNI_OnLoad"));
  if (!fn) {
    __android_log_print(ANDROID_LOG_ERROR, TAG, "JNI_OnLoad missing in real libmain");
    return JNI_ERR;
  }
  return fn(vm, reserved);
}
