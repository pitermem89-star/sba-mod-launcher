package dev.bearite.launcher

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import java.io.File

/** Messages for the UI. Both callbacks are called on the main thread. */
object PatchStatus {
    @Volatile var onChange: ((String) -> Unit)? = null      // one line of the patch log
    @Volatile var onResult: ((Boolean) -> Unit)? = null     // install finished: true = ok
    val main = Handler(Looper.getMainLooper())

    fun post(text: String) { main.post { onChange?.invoke(text) } }
    fun result(ok: Boolean) { main.post { onResult?.invoke(ok) } }
}

object Installer {
    private const val PKG = "com.Earthkwak.Platformer"
    const val ACTION = "dev.bearite.launcher.INSTALL_RESULT"

    /** Android blocks installs from unknown sources until the user allows it for this app. */
    fun canInstall(ctx: Context) = ctx.packageManager.canRequestPackageInstalls()

    fun openInstallPermissionSettings(ctx: Context) {
        ctx.startActivity(
            Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${ctx.packageName}"))
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }

    /** One session with base + all splits, the system asks the user to confirm. */
    fun install(ctx: Context, apks: List<File>) {
        val installer = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(PKG)
        val id = installer.createSession(params)
        installer.openSession(id).use { session ->
            apks.forEachIndexed { i, file ->
                file.inputStream().use { input ->
                    session.openWrite("apk_$i.apk", 0, file.length()).use { out ->
                        input.copyTo(out)
                        session.fsync(out)
                    }
                }
            }
            val intent = Intent(ctx, InstallReceiver::class.java).setAction(ACTION)
            val flags = PendingIntent.FLAG_UPDATE_CURRENT or
                if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
            val pending = PendingIntent.getBroadcast(ctx, id, intent, flags)
            session.commit(pending.intentSender)
        }
    }

    /** The original (Google-signed) game must be removed first: Android refuses a different signature. */
    fun uninstallOriginal(ctx: Context) {
        ctx.startActivity(
            Intent(Intent.ACTION_DELETE, Uri.parse("package:$PKG")).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
    }
}

class InstallReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, -999)) {
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val confirm = if (Build.VERSION.SDK_INT >= 33) {
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                } else {
                    @Suppress("DEPRECATION") intent.getParcelableExtra(Intent.EXTRA_INTENT)
                }
                if (confirm != null) {
                    confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    ctx.startActivity(confirm)
                }
            }
            PackageInstaller.STATUS_SUCCESS -> {
                PatchStatus.post("Игра установлена. Можно запускать.")
                PatchStatus.result(true)
            }
            PackageInstaller.STATUS_FAILURE_INCOMPATIBLE, PackageInstaller.STATUS_FAILURE_CONFLICT -> {
                PatchStatus.post("Установка не прошла: оригинальная игра ещё установлена (другая подпись). Удали её и повтори.")
                PatchStatus.result(false)
            }
            else -> {
                val msg = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE) ?: "код $status"
                PatchStatus.post("Ошибка установки: $msg")
                PatchStatus.result(false)
            }
        }
    }
}
