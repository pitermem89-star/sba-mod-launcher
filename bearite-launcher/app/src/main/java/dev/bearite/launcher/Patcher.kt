package dev.bearite.launcher

import android.content.Context
import java.io.File

object Patcher {

    fun patchAndInstall(context: Context, onStatusUpdate: (String) -> Unit): Boolean {
        try {
            onStatusUpdate("Проверка директории для модов...")
            
            // Легальный путь без Root: /sdcard/Android/data/dev.bearite.launcher/files/
            val baseDir = context.getExternalFilesDir(null)
            if (baseDir == null) {
                onStatusUpdate("Ошибка: Нет доступа к внутренней памяти устройства.")
                return false
            }

            // Создаем цепочку папок: Bearite/mods внутри папки лаунчера
            val beariteDir = File(baseDir, "Bearite")
            val modsDir = File(beariteDir, "mods")

            // Создаем абсолютно пустую папку, если её ещё нет
            if (!modsDir.exists()) {
                val created = modsDir.mkdirs()
                if (created) {
                    onStatusUpdate("Успешно! Пустая папка создана по пути:\nAndroid/data/dev.bearite.launcher/files/Bearite/mods/")
                    return true
                } else {
                    onStatusUpdate("Ошибка: Не удалось создать папку модов.")
                    return false
                }
            }

            // Если папка уже существует и просто нажали кнопку еще раз
            onStatusUpdate("Папка уже готова и ждет файлы:\nAndroid/data/dev.bearite.launcher/files/Bearite/mods/")
            return true

        } catch (e: Exception) {
            e.printStackTrace()
            onStatusUpdate("Критическая ошибка: ${e.localizedMessage}")
            return false
        }
    }
}
