package dev.bearite.launcher

import android.app.Activity
import android.content.ContentValues
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.io.File
import java.util.zip.ZipFile

class MainActivity : Activity() {

    private val gamePackage = "com.Earthkwak.Platformer"

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this)
        root.orientation = LinearLayout.VERTICAL
        root.setPadding(40, 80, 40, 40)

        val status = TextView(this)
        status.textSize = 15f
        status.text = gameInfo()

        val button = Button(this)
        button.text = "Export libmain.so to Downloads"
        button.setOnClickListener {
            status.text = try {
                exportLibMain()
            } catch (e: Exception) {
                "Error: $e"
            }
        }

        root.addView(button)
        root.addView(status)
        setContentView(root)
    }

    private fun gameInfo(): String {
        return try {
            val info = packageManager.getPackageInfo(gamePackage, 0)
            "Game found: " + gamePackage + "\nVersion: " + info.versionName
        } catch (e: PackageManager.NameNotFoundException) {
            "Game not found: $gamePackage"
        }
    }

    private fun exportLibMain(): String {
        if (Build.VERSION.SDK_INT < 29) return "Android 10+ required"
        val info = packageManager.getPackageInfo(gamePackage, 0)
        val app = info.applicationInfo ?: return "No app info"
        val dirs = app.splitSourceDirs ?: return "No splits"
        for (p in dirs) {
            val zip = ZipFile(File(p))
            val entry = zip.getEntry("lib/arm64-v8a/libmain.so")
            if (entry == null) {
                zip.close()
                continue
            }
            val values = ContentValues()
            values.put(MediaStore.Downloads.DISPLAY_NAME, "libmain.so")
            values.put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream")
            val uri = contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            if (uri == null) {
                zip.close()
                return "Cannot create file"
            }
            contentResolver.openOutputStream(uri)?.use { out ->
                zip.getInputStream(entry).use { it.copyTo(out) }
            }
            zip.close()
            return "Saved libmain.so to Downloads"
        }
        return "libmain.so not found"
    }
}
