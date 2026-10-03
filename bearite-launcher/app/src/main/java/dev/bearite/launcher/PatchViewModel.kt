package dev.bearite.launcher

import android.app.Application
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.AndroidViewModel
import java.io.File
import kotlin.concurrent.thread

/**
 * Same flow as the old patcher:
 *   Idle -> Patching -> Patched (APKs are ready, original still installed)
 *        -> user removes the original game -> Installing -> Done
 */
enum class Stage { Idle, Patching, Patched, Installing, Done, Failed }

class PatchViewModel(app: Application) : AndroidViewModel(app) {

    val log = mutableStateListOf<String>()
    var stage by mutableStateOf(Stage.Idle)
        private set

    private var apks: List<File> = emptyList()

    init {
        PatchStatus.onChange = { log.add(it) }
        PatchStatus.onResult = { ok -> stage = if (ok) Stage.Done else Stage.Patched }
    }

    override fun onCleared() {
        PatchStatus.onChange = null
        PatchStatus.onResult = null
    }

    private fun setStage(s: Stage) = PatchStatus.main.post { stage = s }

    /** Button "Патчить игру". Builds and signs the APKs, nothing is installed yet. */
    fun patch() {
        if (stage == Stage.Patching || stage == Stage.Installing) return
        val ctx = getApplication<Application>()
        log.clear()
        stage = Stage.Patching
        thread {
            try {
                apks = Patcher.patch(ctx) { PatchStatus.post(it) }
                PatchStatus.post("Патч готов. Теперь удали оригинальную игру и установи пропатченную.")
                setStage(Stage.Patched)
            } catch (e: Exception) {
                PatchStatus.post("Ошибка: ${e.message ?: e.javaClass.simpleName}")
                setStage(Stage.Failed)
            }
        }
    }

    /** Step 1 after patching: the system "delete app" dialog. */
    fun uninstallOriginal() {
        PatchStatus.post("Подтверди удаление игры в системном окне.")
        Installer.uninstallOriginal(getApplication())
    }

    /** Step 2: installs the patched APKs. */
    fun installPatched() {
        val ctx = getApplication<Application>()
        if (apks.isEmpty() || apks.any { !it.exists() }) {
            PatchStatus.post("Пропатченные файлы пропали, нажми «Патчить игру» ещё раз.")
            stage = Stage.Idle
            return
        }
        if (!Installer.canInstall(ctx)) {
            PatchStatus.post("Разреши лаунчеру устанавливать приложения и вернись сюда.")
            Installer.openInstallPermissionSettings(ctx)
            return
        }
        stage = Stage.Installing
        PatchStatus.post("Установка...")
        thread {
            try {
                Installer.install(ctx, apks)
            } catch (e: Exception) {
                PatchStatus.post("Ошибка установки: ${e.message ?: e.javaClass.simpleName}")
                setStage(Stage.Patched)
            }
        }
    }
}
