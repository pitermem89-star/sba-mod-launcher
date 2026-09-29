package dev.bearite.launcher

import android.content.Context

object Diag {
    fun run(ctx: Context): String {
        return try {
            PairipScan.run(ctx)
        } catch (e: Throwable) {
            "Scan error: $e"
        }
    }
}
