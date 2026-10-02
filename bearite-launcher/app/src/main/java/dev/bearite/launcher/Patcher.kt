package dev.bearite.launcher

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import androidx.core.content.FileProvider
import java.io.File
import java.io.DataOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

object Patcher {

    private const val GAME_PACKAGE = "com.Earthkwak.Platformer"

    // Функция выполнения Root-команд для бэкапа сохранений
    private fun runRootCommand(command: String): Boolean {
        return try {
            val process = Runtime.getRuntime().exec("su")
            val os = DataOutputStream(process.outputStream)
            os.writeBytes("$command\n")
            os.writeBytes("exit\n")
            os.flush()
            process.waitFor() == 0
        } catch (e: Exception) {
            false
        }
    }

    fun patchAndInstall(context: Context, onStatusUpdate: (String) -> Unit): Boolean {
        try {
            onStatusUpdate("Проверка Root для сохранения данных...")
            val hasRoot = runRootCommand("echo 'root check'")
            
            onStatusUpdate("Поиск оригинальной игры...")
            val packageInfo = context.packageManager.getPackageInfo(GAME_PACKAGE, 0)
            val originalApkPath = packageInfo.applicationInfo.sourceDir
            val originalApk = File(originalApkPath)

            // Создаем файлы во внешнем кэше, чтобы они не удалились вместе с лаунчером
            val outputDir = context.getExternalCacheDir() ?: context.cacheDir
            val patchedApk = File(outputDir, "patched_game.apk")
            
            val modsDir = File(context.getExternalFilesDir(null), "mods")
            if (!modsDir.exists()) modsDir.mkdirs()

            val customLibMain = File(modsDir, "libmain.so")
            val customLibBearite = File(modsDir, "libbearite.so")

            if (!customLibMain.exists() || !customLibBearite.exists()) {
                onStatusUpdate("Ошибка! Положите файлы мода в папку:\nAndroid/data/dev.bearite.launcher/files/mods/")
                return false
            }

            // БЭКАП: Если рут есть, тихо сохраняем прогресс пользователя
            if (hasRoot) {
                onStatusUpdate("Root найден. Создаем бэкап сохранений...")
                val backupDir = File(context.filesDir, "game_backup")
                if (!backupDir.exists()) backupDir.mkdirs()
                runRootCommand("tar -cf ${backupDir.absolutePath}/data.tar -C /data/data/$GAME_PACKAGE .")
            } else {
                onStatusUpdate("Предупреждение: Root не найден, сохранения могут быть удалены.")
            }

            // ШАГ 1: Сборка патченого APK в фоне
            onStatusUpdate("Сборка патча (Инжект сошек)...")
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

                    // Вшиваем libbearite.so во все папки архитектур
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

            // ШАГ 2: Вызов системного окна ДЛЯ УДАЛЕНИЯ старой игры
            onStatusUpdate("Открытие окна удаления оригинальной игры...")
            val uninstallIntent = Intent(Intent.ACTION_UNINSTALL_PACKAGE).apply {
                data = Uri.parse("package:$GAME_PACKAGE")
                putExtra(Intent.EXTRA_RETURN_RESULT, true)
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            }
            context.startActivity(uninstallIntent)

            // ШАГ 3: Ждем удаления и запускаем установку патченого APK
            // Поскольку удаление происходит в системном окне, запускаем отложенный поток установки через 7 секунд
            onStatusUpdate("Ожидание удаления... Приготовьтесь к установке мода.")
            Handler(Looper.getMainLooper()).postDelayed({
                onStatusUpdate("Запуск установки модифицированной игры...")
                
                val installIntent = Intent(Intent.ACTION_VIEW).apply {
                    val apkUri: Uri = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N) {
                        FileProvider.getUriForFile(context, "${context.packageName}.provider", patchedApk)
                    } else {
                        Uri.fromFile(patchedApk)
                    }
                    setDataAndType(apkUri, "application/vnd.android.package-archive")
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                }
                context.startActivity(installIntent)

                // ШАГ 4: Если был сделан бэкап, возвращаем файлы на место после установки
                if (hasRoot) {
                    Handler(Looper.getMainLooper()).postDelayed({
                        onStatusUpdate("Восстановление сохранений из бэкапа...")
                        val backupDir = File(context.filesDir, "game_backup")
                        runRootCommand("mkdir -p /data/data/$GAME_PACKAGE")
                        runRootCommand("tar -xf ${backupDir.absolutePath}/data.tar -C /data/data/$GAME_PACKAGE")
                        runRootCommand("chown -R $(stat -c '%U:%G' /data/data/$GAME_PACKAGE) /data/data/$GAME_PACKAGE")
                        onStatusUpdate("Все готово! Модифицированная игра установлена.")
                    }, 5000) // Ждем 5 секунд, пока пользователь завершит установку
                }

            }, 7000) // 7 секунд задержки, чтобы пользователь успел нажать «Удалить»

            return true

        } catch (e: Exception) {
            e.printStackTrace()
            onStatusUpdate("Критическая ошибка: ${e.localizedMessage}")
            return false
        }
    }
}
