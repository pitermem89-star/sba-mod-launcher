package dev.bearite.launcher.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.ListItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import dev.bearite.launcher.GAME_PACKAGE
import dev.bearite.launcher.GameInfo

@Composable
fun HomeScreen(game: GameInfo, onLaunch: () -> Unit) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        ElevatedCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text("Super Bear Adventure", style = MaterialTheme.typography.headlineSmall)
                Text(
                    if (game.installed) "Version ${game.versionName ?: "unknown"}" else "Game is not installed",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                AssistChip(onClick = {}, label = { Text(if (game.installed) "Installed" else "Not installed") })
            }
        }

        Button(
            onClick = onLaunch,
            enabled = game.installed,
            modifier = Modifier.fillMaxWidth().height(56.dp),
        ) {
            Icon(Icons.Filled.PlayArrow, contentDescription = null)
            Spacer(Modifier.size(8.dp))
            Text("Launch game")
        }

        // Stage 2 will put the patch + install flow behind this button.
        FilledTonalButton(onClick = {}, enabled = false, modifier = Modifier.fillMaxWidth().height(56.dp)) {
            Text("Patch and install (next stage)")
        }

        ElevatedCard(Modifier.fillMaxWidth()) {
            Column(Modifier.padding(20.dp), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text("Bearite loader", style = MaterialTheme.typography.titleMedium)
                Text("ABI 2 · package $GAME_PACKAGE", style = MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable
fun PlaceholderScreen(text: String) {
    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, style = MaterialTheme.typography.bodyLarge, color = MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

@Composable
fun SettingsScreen() {
    Column(Modifier.fillMaxSize()) {
        ListItem(headlineContent = { Text("Launcher") }, supportingContent = { Text("Version 0.1.0") })
        ListItem(headlineContent = { Text("Target game") }, supportingContent = { Text(GAME_PACKAGE) })
        ListItem(headlineContent = { Text("Theme") }, supportingContent = { Text("Follows the system (Material You on Android 12+)") })
    }
}
