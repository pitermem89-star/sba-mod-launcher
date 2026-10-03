package dev.bearite.launcher.ui

import android.widget.Toast
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.List
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Info
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import dev.bearite.launcher.launchGame
import dev.bearite.launcher.readGame

private enum class Tab(val title: String, val icon: ImageVector) {
    Home("Home", Icons.Filled.Home),
    Mods("Mods", Icons.AutoMirrored.Filled.List),
    Logs("Logs", Icons.Filled.Info),
    Settings("Settings", Icons.Filled.Settings),
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BeariteApp() {
    val ctx = LocalContext.current
    var tabIndex by rememberSaveable { mutableIntStateOf(0) }
    var game by remember { mutableStateOf(readGame(ctx)) }

    // Re-check the game when the user comes back (e.g. after installing it).
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) { game = readGame(ctx) }

    Scaffold(
        topBar = { CenterAlignedTopAppBar(title = { Text(Tab.entries[tabIndex].title) }) },
        bottomBar = {
            NavigationBar {
                Tab.entries.forEachIndexed { i, tab ->
                    NavigationBarItem(
                        selected = i == tabIndex,
                        onClick = { tabIndex = i },
                        icon = { Icon(tab.icon, contentDescription = tab.title) },
                        label = { Text(tab.title) },
                    )
                }
            }
        },
    ) { padding ->
        Box(Modifier.padding(padding).fillMaxSize()) {
            when (Tab.entries[tabIndex]) {
                Tab.Home -> PatchScreen(game, onLaunch = {
                    if (!launchGame(ctx)) Toast.makeText(ctx, "Game is not installed", Toast.LENGTH_SHORT).show()
                })
                Tab.Mods -> PlaceholderScreen("Installed mods will appear here")
                Tab.Logs -> PlaceholderScreen("bearite.log viewer will appear here")
                Tab.Settings -> SettingsScreen()
            }
        }
    }
}
