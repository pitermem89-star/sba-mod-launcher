package dev.bearite.launcher

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.view.View
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.progressindicator.LinearProgressIndicator

class MainActivity : AppCompatActivity() {

    private lateinit var status: TextView
    private lateinit var patcher: Patcher
    private lateinit var progress: LinearProgressIndicator
    private lateinit var buttons: List<MaterialButton>

    // While something runs: show the progress bar and block the buttons.
    private var busy = false
        set(value) {
            field = value
            if (::progress.isInitialized) {
                progress.visibility = if (value) View.VISIBLE else View.INVISIBLE
                buttons.forEach { it.isEnabled = !value }
            }
        }

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

        setContentView(R.layout.activity_main)
        progress = findViewById(R.id.progress)
        status = findViewById(R.id.status)
        status.text = ""
        val patchButton = findViewById<MaterialButton>(R.id.btn_patch)
        val installButton = findViewById<MaterialButton>(R.id.btn_install)
        val diagButton = findViewById<MaterialButton>(R.id.btn_diag)
        buttons = listOf(patchButton, installButton, diagButton)
        patchButton.setOnClickListener { startPatch() }
        installButton.setOnClickListener { installPrepared() }
        diagButton.setOnClickListener { startDiag() }

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

    private fun startDiag() {
        if (busy) return
        busy = true
        status.text = "Scanning game, please wait...\n"
        Thread {
            val result = try {
                Diag.run(this)
            } catch (e: Throwable) {
                "Diag error: $e"
            }
            runOnUiThread {
                busy = false
                status.text = result
            }
        }.start()
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
