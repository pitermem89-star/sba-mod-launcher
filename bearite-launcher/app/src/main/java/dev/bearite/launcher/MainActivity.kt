package dev.bearite.launcher

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import dev.bearite.launcher.ui.BeariteApp
import dev.bearite.launcher.ui.BeariteTheme

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            BeariteTheme {
                BeariteApp()
            }
        }
    }
}
