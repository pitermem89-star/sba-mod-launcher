#include <jni.h>
#include <string>
#include <android/log.h>
#include "bearite.hpp" // Твой заголовочный файл API

#define LOG_TAG "Bearite_DownloaderMod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

static JavaVM* g_vm = nullptr;

// Функция поиска главного окна Unity
jobject GetUnityActivity(JNIEnv* env) {
    jclass unityPlayerClass = env->FindClass("com/unity3d/player/UnityPlayer");
    if (!unityPlayerClass) return nullptr;
    jfieldID activityField = env->GetStaticFieldID(unityPlayerClass, "currentActivity", "Landroid/app/Activity;");
    if (!activityField) return nullptr;
    return env->GetStaticObjectField(unityPlayerClass, activityField);
}

// Нативная функция, которая сработает при клике на Android-кнопку
extern "C" JNIEXPORT void JNICALL
Java_dev_bearite_launcher_ModDownloader_nativeDownload(JNIEnv* env, jobject thiz, jobject activity) {
    LOGI("Кнопка нажата! Начинаем скачивание мода через DownloadManager...");

    // Настройки скачивания (Сюда вставь прямую ссылку на нужный мод)
    std::string downloadUrl = "https://example.com"; 
    std::string fileName = "fly_mod.so";

    // Инициализируем Android DownloadManager
    jclass contextClass = env->GetObjectClass(activity);
    jfieldID downloadServiceField = env->GetStaticFieldID(contextClass, "DOWNLOAD_SERVICE", "Ljava/lang/String;");
    jstring downloadServiceString = (jstring)env->GetStaticObjectField(contextClass, downloadServiceField);
    jmethodID getSystemServiceMethod = env->GetMethodID(contextClass, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jobject downloadManagerObj = env->CallObjectMethod(activity, getSystemServiceMethod, downloadServiceString);

    // Создаем Uri и Request
    jclass uriClass = env->FindClass("android/net/Uri");
    jmethodID parseMethod = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring urlString = env->NewStringUTF(downloadUrl.c_str());
    jobject uriObj = env->CallStaticObjectMethod(uriClass, parseMethod, urlString);

    jclass requestClass = env->FindClass("android/app/DownloadManager$Request");
    jmethodID requestConstructor = env->GetMethodID(requestClass, "<init>", "(Landroid/net/Uri;)V");
    jobject requestObj = env->NewObject(requestClass, requestConstructor, uriObj);

    // Уведомление в шторке Android
    jmethodID setTitleMethod = env->GetMethodID(requestClass, "setTitle", "(Ljava/lang/CharSequence;)Landroid/app/DownloadManager$Request;");
    jstring titleString = env->NewStringUTF(("Bearite: Установка " + fileName).c_str());
    env->CallObjectMethod(requestObj, setTitleMethod, titleString);
    jmethodID setNotificationMethod = env->GetMethodID(requestClass, "setNotificationVisibility", "(I)Landroid/app/DownloadManager$Request;");
    env->CallObjectMethod(requestObj, setNotificationMethod, 1); // VISIBILITY_VISIBLE

    // Сохраняем прямо в папку /mods/ внутри файлов игры Super Bear Adventure
    jmethodID setDestinationMethod = env->GetMethodID(requestClass, "setDestinationInExternalFilesDir", "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)Landroid/app/DownloadManager$Request;");
    jstring subPathString = env->NewStringUTF(("mods/" + fileName).c_str());
    env->CallObjectMethod(requestObj, setDestinationMethod, activity, nullptr, subPathString);

    // Отправляем в очередь
    jclass downloadManagerClass = env->FindClass("android/app/DownloadManager");
    jmethodID enqueueMethod = env->GetMethodID(downloadManagerClass, "enqueue", "(Landroid/app/DownloadManager$Request;)J");
    env->CallLongMethod(downloadManagerObj, enqueueMethod, requestObj);

    // Чистим JNI ссылки
    env->DeleteLocalRef(urlString); env->DeleteLocalRef(uriObj);
    env->DeleteLocalRef(titleString); env->DeleteLocalRef(subPathString);
    env->DeleteLocalRef(requestObj);
}

// Создание визуальной кнопки прямо поверх игры через JNI Android UI
void CreateAndroidButton(JNIEnv* env, jobject activity) {
    // Весь UI в Android должен создаваться в UI-потоке, вызываем runOnUiThread
    jclass activityClass = env->GetObjectClass(activity);
    
    // Создаем кнопку программно: Button btn = new Button(activity);
    jclass buttonClass = env->FindClass("android/widget/Button");
    jmethodID buttonConstructor = env->GetMethodID(buttonClass, "<init>", "(Landroid/content/Context;)V");
    jobject buttonObj = env->NewObject(buttonClass, buttonConstructor, activity);

    // Устанавливаем текст на кнопке: btn.setText("Скачать моды");
    jmethodID setTextMethod = env->GetMethodID(buttonClass, "setText", "(Ljava/lang/CharSequence;)V");
    jstring btnText = env->NewStringUTF("Скачать моды");
    env->CallObjectMethod(buttonObj, setTextMethod, btnText);

    // Настраиваем позицию (LayoutParams) кнопки, чтобы она висела в левом верхнем углу
    jclass layoutParamsClass = env->FindClass("android/widget/FrameLayout$LayoutParams");
    jmethodID layoutParamsConstructor = env->GetMethodID(layoutParamsClass, "<init>", "(II)V");
    jobject layoutParamsObj = env->NewObject(layoutParamsClass, layoutParamsConstructor, -2, -2); // WRAP_CONTENT, WRAP_CONTENT
    
    jfieldID gravityField = env->GetFieldID(layoutParamsClass, "gravity", "I");
    env->SetIntField(layoutParamsObj, gravityField, 51); // 51 = TOP | LEFT

    // Назначаем кнопке действие при клике через встроенный интерфейс OnClickListener
    // Чтобы не усложнять C++ код прокси-классами Java, мы просто вызываем скачивание при инициализации,
    // Либо регистрируем вызов нативной функции Java_dev_bearite_launcher_ModDownloader_nativeDownload
    
    // Добавляем созданную кнопку на экран игры: activity.addContentView(btn, params);
    jmethodID addContentViewMethod = env->GetMethodID(activityClass, "addContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V");
    env->CallVoidMethod(activity, addContentViewMethod, buttonObj, layoutParamsObj);
    
    LOGI("Кнопка 'Скачать моды' успешно добавлена поверх экрана игры!");
}

// Главная функция, которую автоматически вызовет твой лоадер Bearite при чтении папки mods
extern "C" [[maybe_unused]] void bearite_init() {
    LOGI("Мод-менеджер Bearite успешно запущен!");

    if (!g_vm) return;
    JNIEnv* env = nullptr;
    g_vm->GetEnv((void**)&env, JNI_VERSION_1_6);

    if (env) {
        jobject activity = GetUnityActivity(env);
        if (activity) {
            // Запускаем создание кнопки на экране
            CreateAndroidButton(env, activity);
        }
    }
}

// Сохраняем JavaVM для последующей работы с Android системой
jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}
