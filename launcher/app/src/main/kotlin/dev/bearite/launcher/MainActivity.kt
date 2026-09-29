package dev.bearite.launcher

import android.app.Activity
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {

    private val gamePackage = "com.Earthkwak.Platformer"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val text = TextView(this)
        text.textSize = 15f
        text.setPadding(40, 80, 40, 40)
        text.setTextIsSelectable(true)
        text.text = describeGame()
        val scroll = ScrollView(this)
        scroll.addView(text)
        setContentView(scroll)
    }

    private fun describeGame(): String {
        val info = try {
            packageManager.getPackageInfo(gamePackage, 0)
        } catch (e: PackageManager.NameNotFoundException) {
            return "Game not found: $gamePackage\nInstall Super Bear Adventure from Google Play first."
        }
        val app = info.applicationInfo ?: return "Game found, but no app info."
        val sb = StringBuilder()
        sb.append("Game found: ").append(gamePackage).append('\n')
        sb.append("Version: ").append(info.versionName).append('\n')
        sb.append("Device ABIs: ").append(Build.SUPPORTED_ABIS.joinToString()).append("\n\n")
        sb.append("base APK:\n").append(app.sourceDir).append("\n\n")
        val splits = app.splitSourceDirs
        if (splits.isNullOrEmpty()) {
            sb.append("No split APKs.\n")
        } else {
            sb.append("Split APKs:\n")
            for (s in splits) sb.append(s).append('\n')
        }
        sb.append("\nNative lib dir:\n").append(app.nativeLibraryDir)
        return sb.toString()
    }
}
