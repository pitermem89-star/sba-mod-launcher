package dev.bearite.launcher.ui

import android.os.Build
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext

// Fallback "honey bear" palette for Android 11 and older (no Material You).
private val LightColors = lightColorScheme(
    primary = Color(0xFF8B5000),
    secondary = Color(0xFF725A42),
    tertiary = Color(0xFF58642F),
)
private val DarkColors = darkColorScheme(
    primary = Color(0xFFFFB86B),
    secondary = Color(0xFFE0C1A3),
    tertiary = Color(0xFFBFCC94),
)

@Composable
fun BeariteTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val ctx = LocalContext.current
    val scheme = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S ->
            if (dark) dynamicDarkColorScheme(ctx) else dynamicLightColorScheme(ctx)
        dark -> DarkColors
        else -> LightColors
    }
    MaterialTheme(colorScheme = scheme, content = content)
}
