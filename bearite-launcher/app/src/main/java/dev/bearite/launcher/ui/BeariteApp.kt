package dev.bearite.launcher.ui

import android.widget.Toast
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.List
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.compose.LifecycleEventEffect
import dev.bearite.launcher.GameInfo
import dev.bearite.launcher.Patcher
import dev.bearite.launcher.launchGame
import dev.bearite.launcher.readGame
import kotlinx.coroutines.launch

private enum class Tab(val title: String, val icon: androidx.compose.ui.graphics.vector.ImageVector) {
    Home("Home", Icons.Filled.Home),
    Mods("Mods", Icons.Filled.List),
    Logs("Logs", Icons.Filled.List),
    Settings("Settings", Icons.Filled.Settings)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun BeariteApp() {
    val context = LocalContext.current
    var selectedTab by rememberSaveable { mutableStateOf(Tab.Home) }
    var game by remember { mutableStateOf(readGame(context)) }
    val scope = rememberCoroutineScope()
    var patchStatus by remember { mutableStateOf("Готов к работе") }

    // Обновляем статус игры каждый раз, когда лаунчер возвращается на экран
    LifecycleEventEffect(Lifecycle.Event.ON_RESUME) {
        game = readGame(context)
    }

    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(title = { Text("Bearite Launcher") })
        },
        bottomBar = {
            NavigationBar {
                Tab.values().forEach { tab ->
                    NavigationBarItem(
                        selected = selectedTab == tab,
                        onClick = { selectedTab = tab },
                        icon = { Icon(tab.icon, contentDescription = tab.title) },
                        label = { Text(tab.title) }
                    )
                }
            }
        }
    ) { padding ->
        Box(modifier = Modifier.padding(padding).fillMaxSize()) {
            when (selectedTab) {
                Tab.Home -> HomeScreen(
                    game = game, 
                    patchStatus = patchStatus,
                    onLaunch = { launchGame(context) },
                    onPatch = {
                        scope.launch {
                            Toast.makeText(context, "Начался процесс сборки...", Toast.LENGTH_SHORT).show()
                            Patcher.patchAndInstall(context) { status ->
                                patchStatus = status
                            }
                        }
                    }
                )
                Tab.Mods -> PlaceholderScreen("Установленные моды будут здесь")
                Tab.Logs -> PlaceholderScreen(patchStatus)
                Tab.Settings -> SettingsScreen()
            }
        }
    }
}

@Composable
fun HomeScreen(
    game: GameInfo, 
    patchStatus: String,
    onLaunch: () -> Unit, 
    onPatch: () -> Unit
) {
    // Отслеживание процесса, чтобы заблокировать кнопку от двойных нажатий
    var isPatching by remember { mutableStateOf(false) }

    // Автоматический сброс анимации загрузки, если патчер вернул финальный статус
    LaunchedEffect(patchStatus) {
        if (patchStatus.contains("успешно") || patchStatus.contains("Ошибка")) {
            isPatching = false
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // Карточка информации о целевой игре
        ElevatedCard(modifier = Modifier.fillMaxWidth().padding(horizontal = 20.dp, vertical = 10.dp)) {
            Column(modifier = Modifier.padding(20.dp)) {
                Text("Super Bear Adventure", style = MaterialTheme.typography.titleMedium)
                if (game.installed) {
                    Text("Версия: ${game.versionName}", style = MaterialTheme.typography.bodyMedium)
                } else {
                    Text("Игра не установлена", style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.error)
                }
            }
        }

        // Кнопка запуска ванильной (оригинальной) игры
        Button(
            onClick = onLaunch,
            enabled = game.installed && !isPatching,
            modifier = Modifier.fillMaxWidth().height(56.dp).padding(horizontal = 20.dp)
        ) {
            Icon(Icons.Filled.PlayArrow, contentDescription = null)
            Spacer(Modifier.size(8.dp))
            Text("Запустить игру")
        }

        // АКТИВНАЯ КНОПКА СБОРКИ ПАТЧА
        FilledTonalButton(
            onClick = {
                isPatching = true
                onPatch()
            },
            // Разблокируется сама, если игра установлена на устройстве
            enabled = game.installed && !isPatching,
            modifier = Modifier.fillMaxWidth().height(56.dp).padding(horizontal = 20.dp)
        ) {
            if (isPatching) {
                CircularProgressIndicator(
                    modifier = Modifier.size(24.dp),
                    color = MaterialTheme.colorScheme.primary,
                    strokeWidth = 2.dp
                )
                Spacer(Modifier.size(12.dp))
                Text("Патчинг файлов...")
            } else {
                Text("Patch and install (next stage)")
            }
        }

        // СТРОКА ГОТОВНОСТИ (PROGRESS BAR) И ЛОГИ СБОРКИ
        ElevatedCard(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 20.dp)
        ) {
            Column(modifier = Modifier.padding(20.dp)) {
                Text("Статус операции:", style = MaterialTheme.typography.titleSmall)
                Spacer(Modifier.height(8.dp))
                
                // Линейный индикатор: бежит во время процесса слияния, замирает по окончании
                if (isPatching && !patchStatus.contains("успешно") && !patchStatus.contains("Ошибка")) {
                    LinearProgressIndicator(
                        modifier = Modifier.fillMaxWidth(),
                        color = MaterialTheme.colorScheme.primary
                    )
                } else {
                    LinearProgressIndicator(
                        progress = { 1f },
                        modifier = Modifier.fillMaxWidth(),
                        color = MaterialTheme.colorScheme.outlineVariant
                    )
                }
                
                Spacer(Modifier.height(12.dp))
                
                // Текстовое поле вывода логов
                Text(
                    text = patchStatus,
                    style = MaterialTheme.typography.bodySmall,
                    color = if (patchStatus.contains("Ошибка")) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}

@Composable
fun PlaceholderScreen(text: String) {
    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Text(text, style = MaterialTheme.typography.bodyLarge)
    }
}

@Composable
fun SettingsScreen() {
    Column(modifier = Modifier.fillMaxSize().padding(16.dp)) {
        ListItem(headlineContent = { Text("Лаунчер: Bearite") })
        ListItem(headlineContent = { Text("Цель: com.Earthkwak.Platformer") })
        ListItem(headlineContent = { Text("Версия: 0.1.0") })
    }
}
