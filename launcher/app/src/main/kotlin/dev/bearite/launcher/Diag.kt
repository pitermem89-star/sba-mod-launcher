package dev.bearite.launcher

import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import java.io.File
import java.util.zip.ZipFile

object Diag {

    private val KEYWORDS = listOf(
        "pairip",
        "com.android.vending",
        "getInstallerPackageName",
        "getInstallSourceInfo",
        "icense",
        "IntegrityManager",
        "Get this app from"
    )

    fun run(ctx: Context): String {
        val sb = StringBuilder()
        val pm = ctx.packageManager
        val info = try {
            pm.getPackageInfo(Patcher.GAME, PackageManager.GET_ACTIVITIES)
        } catch (e: Exception) {
            return "Game not installed."
        }
        val app = info.applicationInfo ?: return "No app info."

        sb.append("Application class: ").append(app.className).append('\n')
        val launch = pm.getLaunchIntentForPackage(Patcher.GAME)
        sb.append("Launch activity: ").append(launch?.component?.className).append('\n')

        val installer = if (Build.VERSION.SDK_INT >= 30) {
            pm.getInstallSourceInfo(Patcher.GAME).installingPackageName
        } else {
            pm.getInstallerPackageName(Patcher.GAME)
        }
        sb.append("Installer: ").append(installer).append('\n')

        sb.append("Activities:\n")
        info.activities?.take(30)?.forEach { sb.append("  ").append(it.name).append('\n') }

        val files = ArrayList<File>()
        files.add(File(app.sourceDir))
        app.splitSourceDirs?.forEach { files.add(File(it)) }
        for (f in files) {
            scanApk(f, sb)
        }
        return sb.toString()
    }

    private fun scanApk(file: File, sb: StringBuilder) {
        val zip = ZipFile(file)
        try {
            val entries = zip.entries()
            while (entries.hasMoreElements()) {
                val e = entries.nextElement()
                if (!e.name.startsWith("classes") || !e.name.endsWith(".dex")) continue
                val bytes = zip.getInputStream(e).use { it.readBytes() }
                val text = String(bytes, Charsets.ISO_8859_1)
                sb.append("\n== ").append(file.name).append(" / ").append(e.name).append('\n')
                for (kw in KEYWORDS) {
                    var count = 0
                    var first = -1
                    var idx = text.indexOf(kw)
                    while (idx >= 0) {
                        if (first < 0) first = idx
                        count++
                        idx = text.indexOf(kw, idx + kw.length)
                    }
                    if (count > 0) {
                        sb.append("  ").append(kw).append(" x").append(count).append('\n')
                        sb.append("    ").append(snippet(text, first)).append('\n')
                    }
                }
            }
        } finally {
            zip.close()
        }
    }

    private fun snippet(text: String, idx: Int): String {
        val from = maxOf(0, idx - 50)
        val to = minOf(text.length, idx + 70)
        val sb = StringBuilder()
        for (i in from until to) {
            val c = text[i]
            sb.append(if (c.code in 32..126) c else '.')
        }
        return sb.toString()
    }
}
