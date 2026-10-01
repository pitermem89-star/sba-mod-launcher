package dev.bearite.launcher

import android.content.Context
import java.io.File
import java.io.FileOutputStream

class Patcher(private val ctx: Context, private val log: (String) -> Unit) {

    companion object {
        const val GAME = "com.Earthkwak.Platformer"
        // Путь внутри песочницы Phantom, куда мы подкинем наш хук
        private const val LIB_BEARITE = "libbearite.so"
        private const val LIB_MAIN_HOOK = "libmain.so"
    }

    // Теперь рабочая директория — это не папка для сборки APK, а папка с модами
    private val virtualRoot = File(ctx.filesDir, "phantom_root")
    private val readyMarker = File(virtualRoot, "READY")

    fun isGameInstalled(): Boolean {
        return try {
            ctx.packageManager.getPackageInfo(GAME, 0)
            true
        } catch (e: Exception) {
            false
        }
    }

    // Проверка, готовы ли библиотеки мода к работе в песочнице
    fun isEnvReady(): Boolean {
        return readyMarker.exists()
    }

    fun patch() {
        log("Подготовка виртуального окружения модов...")
        
        try {
            if (!virtualRoot.exists()) {
                virtualRoot.mkdirs()
            }

            // Нам больше не нужно извлекать и переподписывать APK игры!
            // Мы просто извлекаем наш мод-меню (libbearite.so) и лоадер-библиотеку (libmain.so) 
            // из ассетов самого лаунчера в изолированную директорию Phantom.
            
            log("Копирование модулей мода...")
            extractAsset(LIB_BEARITE, File(virtualRoot, LIB_BEARITE))
            extractAsset(LIB_MAIN_HOOK, File(virtualRoot, LIB_MAIN_HOOK))

            readyMarker.writeText("ok")
            log("Окружение успешно подготовлено. Пересборка APK не требуется!")
            
        } catch (e: Exception) {
            log("Ошибка при подготовке окружения: ${e.message}")
            e.printStackTrace()
            throw e
        }
    }

    private fun extractAsset(assetName: String, outputFile: File) {
        ctx.assets.open(assetName).use { input ->
            FileOutputStream(outputFile).use { output ->
                input.copyTo(output)
            }
        }
    }

    fun cleanup() {
        virtualRoot.deleteRecursively()
        log("Виртуальное окружение очищено.")
    }
    
    // Функции install(), sign(), rewriteNativeSplit() и работу с KeyStore 
    // мы полностью удаляем, так как Phantom избавляет от необходимости ставить APK!
}
