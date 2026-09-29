// Proxy for the game's libmain.so: loads Bearite, then hands over to the real libmain.
#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#define TAG "bearite"

typedef jint (*OnLoadFn)(JavaVM*, void*);

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  void* mod = dlopen("libbearite.so", RTLD_NOW | RTLD_GLOBAL);
  if (!mod) {
    __android_log_print(ANDROID_LOG_ERROR, TAG, "libbearite.so: %s", dlerror());
  }

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
