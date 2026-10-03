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
            // Лаунчер ищет сошки в своей папке, куда ты их положил
            val baseDir = context.getExternalFilesDir(null) ?: context.filesDir
            val launcherModsDir = File(baseDir, "mods")
            if (!launcherModsDir.exists()) launcherModsDir.mkdirs()

            val customLibMain = File(launcherModsDir, "libmain.so")
            val customLibBearite = File(launcherModsDir, "libbearite.so")

            if (!customLibMain.exists() || !customLibBearite.exists()) {
                onStatusUpdate("Ошибка! Сначала положите libmain.so и libbearite.so лаунчера в:\nAndroid/data/dev.bearite.launcher/files/mods/")
                return false
            }

            onStatusUpdate("Поиск установленной оригинальной игры...")
            val packageInfo = context.packageManager.getPackageInfo(GAME_PACKAGE, 0)
            val originalApkPath = packageInfo.applicationInfo.sourceDir
            val originalApk = File(originalApkPath)

            val outputDir = context.getExternalCacheDir() ?: context.cacheDir
            val patchedApk = File(outputDir, "patched_game.apk")
            if (patchedApk.exists()) patchedApk.delete()

            onStatusUpdate("Сборка пропатченного APK...")
            ZipInputStream(originalApk.inputStream().buffered()).use { zis ->
                ZipOutputStream(patchedApk.outputStream().buffered()).use { zos ->
                    var entry: ZipEntry? = zis.getNextEntry()
                    while (entry != null) {
                        if (!entry.name.startsWith("META-INF/")) {
                            if (entry.name.endsWith("libmain.so")) {
                                zos.putNextEntry(ZipEntry(entry.name))
                                customLibMain.inputStream().use { it.copyTo(zos) }
                            } else {
                                zos.putNextEntry(ZipEntry(entry.name))
                                zis.copyTo(zos)
                            }
                        }
                        zos.closeEntry()
                        entry = zis.getNextEntry()
                    }

                    // Инжектим libbearite.so во все архитектуры
                    val architectures = listOf("arm64-v8a", "armeabi-v7a", "x86", "x86_64")
                    for (arch in architectures) {
                        try {
                            val abiPath = "lib/$arch/libbearite.so"
                            zos.putNextEntry(ZipEntry(abiPath))
                            customLibBearite.inputStream().use { it.copyTo(zos) }
                            zos.closeEntry()
                        } catch (e: Exception) {}
                    }
                }
            }

            onStatusUpdate("Запуск установки пропатченной игры...")
            installApk(context, patchedApk)
            onStatusUpdate("Игра успешно пропатчена! Установите её.")
            return true

        } catch (e: Exception) {
            e.printStackTrace()
            onStatusUpdate("Критическая ошибка: ${e.localizedMessage}")
            return false
        }
    }

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
