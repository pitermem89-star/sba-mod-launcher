package dev.bearite.launcher

import android.app.Application
import com.google.android.material.color.DynamicColors

class App : Application() {
    override fun onCreate() {
        super.onCreate()
        // Android 12+: use the colours of the user's wallpaper (Material You). Older phones use the theme colours.
        DynamicColors.applyToActivitiesIfAvailable(this)
    }
}
