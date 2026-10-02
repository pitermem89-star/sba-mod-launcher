package dev.bearite.launcher

import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager

const val GAME_PACKAGE = "com.Earthkwak.Platformer"

data class GameInfo(val installed: Boolean, val versionName: String?)

fun readGame(ctx: Context): GameInfo = try {
    val info = ctx.packageManager.getPackageInfo(GAME_PACKAGE, 0)
    GameInfo(installed = true, versionName = info.versionName)
} catch (e: PackageManager.NameNotFoundException) {
    GameInfo(installed = false, versionName = null)
}

/** Starts the game. Returns false if it is not installed. */
fun launchGame(ctx: Context): Boolean {
    val intent = ctx.packageManager.getLaunchIntentForPackage(GAME_PACKAGE) ?: return false
    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    ctx.startActivity(intent)
    return true
}
