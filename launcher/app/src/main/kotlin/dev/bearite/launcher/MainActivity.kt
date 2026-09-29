package dev.bearite.launcher

import android.app.Activity
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipFile

class MainActivity : Activity() {

    private val gamePackage = "com.Earthkwak.Platformer"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val text = TextView(this)
        text.textSize = 13f
        text.setPadding(40, 80, 40, 40)
        text.setTextIsSelectable(true)
        text.text = "Scanning..."
        val scroll = ScrollView(this)
        scroll.addView(text)
        setContentView(scroll)
        Thread {
            val result = try {
                describeGame()
            } catch (e: Exception) {
                "Error: $e"
            }
            runOnUiThread { text.text = result }
        }.start()
    }

    private fun describeGame(): String {
        val info = try {
            packageManager.getPackageInfo(gamePackage, 0)
        } catch (e: PackageManager.NameNotFoundException) {
            return "Game not found: $gamePackage"
        }
        val app = info.applicationInfo ?: return "No app info."
        val sb = StringBuilder()
        sb.append("Game ").append(info.versionName).append('\n')
        sb.append("ABIs: ").append(Build.SUPPORTED_ABIS.joinToString()).append("\n\n")
        val paths = ArrayList<String>()
        paths.add(app.sourceDir)
        app.splitSourceDirs?.let { paths.addAll(it) }
        for (p in paths) scanApk(File(p), sb)
        return sb.toString()
    }

    private fun scanApk(file: File, sb: StringBuilder) {
        sb.append("== ").append(file.name).append(" (")
        sb.append(file.length() / 1024).append(" KB)\n")
        try {
            ZipFile(file).use { zip ->
                var other = 0
                val entries = zip.entries()
                while (entries.hasMoreElements()) {
                    val e = entries.nextElement()
                    val n = e.name
                    if (n.endsWith(".so") || n.endsWith(".dex") || n == "AndroidManifest.xml") {
                        val m = if (e.method == ZipEntry.STORED) "STORED" else "DEFLATED"
                        sb.append("  ").append(n).append("  ")
                        sb.append(e.size / 1024).append("KB ").append(m).append('\n')
                    } else {
                        other++
                    }
                }
                sb.append("  (+").append(other).append(" other files)\n")
            }
        } catch (e: Exception) {
            sb.append("  cannot read: ").append(e).append('\n')
        }
        sb.append('\n')
    }
}
