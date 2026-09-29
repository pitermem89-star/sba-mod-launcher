package dev.bearite.launcher

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

class MainActivity : Activity() {

    private lateinit var status: TextView
    private lateinit var patcher: Patcher
    private var busy = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val code = i.getIntExtra(PackageInstaller.EXTRA_STATUS, -1)
            val msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: ""
            if (code == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                val confirm = i.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirm != null) startActivity(confirm)
            } else if (code == PackageInstaller.STATUS_SUCCESS) {
                status.append("DONE: patched game installed.\n")
                patcher.cleanup()
            } else {
                status.append("Install failed ($code): $msg\n")
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        patcher = Patcher(this) { m -> runOnUiThread { status.append(m + "\n") } }

        val root = LinearLayout(this)
        root.orientation = LinearLayout.VERTICAL
        root.setPadding(40, 80, 40, 40)

        val patchButton = Button(this)
        patchButton.text = "Patch and install game"
        patchButton.setOnClickListener { startPatch() }

        val installButton = Button(this)
        installButton.text = "Install already prepared files"
        installButton.setOnClickListener { installPrepared() }

        status = TextView(this)
        status.textSize = 14f
        status.setTextIsSelectable(true)
        status.text = "WARNING: the original game will be uninstalled, local game data is lost.\n\n"

        root.addView(patchButton)
        root.addView(installButton)
        val scroll = ScrollView(this)
        scroll.addView(status)
        root.addView(scroll)
        setContentView(root)

        val filter = IntentFilter(Patcher.ACTION_INSTALL_RESULT)
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        } else {
            registerReceiver(receiver, filter)
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        unregisterReceiver(receiver)
    }

    private fun startPatch() {
        if (busy) return
        if (!patcher.isGameInstalled()) {
            status.append("Game is not installed.\n")
            return
        }
        if (!packageManager.canRequestPackageInstalls()) {
            status.append("Allow installing apps for Bearite Launcher, then press the button again.\n")
            startActivity(
                Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:$packageName"))
            )
            return
        }
        busy = true
        Thread {
            try {
                patcher.patch()
                runOnUiThread {
                    busy = false
                    askUninstall()
                }
            } catch (e: Throwable) {
                runOnUiThread {
                    busy = false
                    status.append("Patch failed: $e\n")
                }
            }
        }.start()
    }

    private fun askUninstall() {
        status.append("Now confirm uninstalling the original game.\n")
        val intent = Intent(Intent.ACTION_DELETE, Uri.parse("package:" + Patcher.GAME))
        intent.putExtra(Intent.EXTRA_RETURN_RESULT, true)
        startActivityForResult(intent, 1)
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 1) return
        if (patcher.isGameInstalled()) {
            status.append("Uninstall cancelled. Prepared files are kept.\n")
        } else {
            installPrepared()
        }
    }

    private fun installPrepared() {
        if (busy) return
        if (patcher.isGameInstalled()) {
            status.append("Uninstall the original game first (press Patch and install).\n")
            return
        }
        busy = true
        Thread {
            try {
                patcher.install()
                runOnUiThread { busy = false }
            } catch (e: Throwable) {
                runOnUiThread {
                    busy = false
                    status.append("Install error: $e\n")
                }
            }
        }.start()
    }
}
