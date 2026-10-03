package dev.bearite.launcher

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import androidx.core.content.FileProvider
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

object Patcher {

    private const val GAME_PACKAGE = "com.Earthkwak.Platformer"

    fun patchAndInstall(context: Context, onStatusUpdate: (String) -> Unit): Boolean {
        try {
            onStatusUpdate("Поиск установленной оригинальной игры...")
            val packageInfo = context.packageManager.getPackageInfo(GAME_PACKAGE, 0)
            val originalApkPath = packageInfo.applicationInfo.sourceDir
            val originalApk = File(originalApkPath)

            // Безопасное создание рабочей директории во внешнем кэше приложения
            val outputDir = context.getExternalCacheDir() ?: context.cacheDir
            val patchedApk = File(outputDir, "patched_game.apk")
            if (patchedApk.exists()) {
                patchedApk.delete()
            }
            
            // Защищенный путь к папке с модами: /sdcard/Android/data/dev.bearite.launcher/files/mods/
            val modsDir = File(context.getExternalFilesDir(null), "mods")
            if (!modsDir.exists()) {
                modsDir.mkdirs()
            }

            val customLibMain = File(modsDir, "libmain.so")
            val customLibBearite = File(modsDir, "libbearite.so")

            // Проверяем, закинул ли пользователь файлы мода на телефон
            if (!customLibMain.exists() || !customLibBearite.exists()) {
                onStatusUpdate("Ошибка! Положите libmain.so и libbearite.so по пути:\nВнутренняя память -> Android/data/dev.bearite.launcher/files/mods/")
                return false
            }

            onStatusUpdate("Инжект нативных библиотек мода в APK...")
            ZipInputStream(originalApk.inputStream().buffered()).use { zis ->
                ZipOutputStream(patchedApk.outputStream().buffered()).use { zos ->
                    var entry: ZipEntry? = zis.getNextEntry()
                    while (entry != null) {
                        // Вырезаем старые подписи оригинального Google Play APK, чтобы Android не ругался
                        if (!entry.name.startsWith("META-INF/")) {
                            if (entry.name.endsWith("libmain.so")) {
                                // Подменяем оригинальный libmain.so на наш загрузчик мода
                                zos.putNextEntry(ZipEntry(entry.name))
                                customLibMain.inputStream().use { it.copyTo(zos) }
                            } else {
                                // Все остальные файлы игры переносим без изменений
                                zos.putNextEntry(ZipEntry(entry.name))
                                zis.copyTo(zos)
                            }
                        }
                        zos.closeEntry()
                        entry = zis.getNextEntry()
                    }

                    // Вшиваем наш главный файл libbearite.so во все папки процессорных архитектур
                    val architectures = listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
                    for (arch in architectures) {
                        try {
                            val abiPath = "lib/$arch/libbearite.so"
                            zos.putNextEntry(ZipEntry(abiPath))
                            customLibBearite.inputStream().use { it.copyTo(zos) }
                            zos.closeEntry()
                        } catch (e: Exception) {
                            // Игнорируем дубликаты, если архитектура уже была записана
                        }
                    }
                }
            }

            onStatusUpdate("Запуск стандартного установщика пакетов...")
            installApk(context, patchedApk)
            onStatusUpdate("Игра успешно пропатчена! Подтвердите установку в системном окне.")
            return true

        } catch (e: Exception) {
            e.printStackTrace()
            onStatusUpdate("Критическая ошибка патчера: ${e.localizedMessage}")
            return false
        }
    }

    // Вызов официального и безопасного окна установки APK через FileProvider без Root
    private fun installApk(context: Context, apkFile: File) {
        val intent = Intent(Intent.ACTION_VIEW).apply {
            val apkUri: Uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                FileProvider.getUriForFile(context, "${context.packageName}.provider", apkFile)
            } else {
                Uri.fromFile(apkFile)
            }
            setDataAndType(apkUri, "application/vnd.android.package-archive")
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }
        context.startActivity(intent)
    }
}
