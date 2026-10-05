package dev.bearite.launcher

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import com.android.apksig.ApkSigner
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream

class Patcher(private val ctx: Context, private val log: (String) -> Unit) {

    companion object {
        const val GAME = "com.Earthkwak.Platformer"
        const val ACTION_INSTALL_RESULT = "dev.bearite.launcher.INSTALL_RESULT"
        private const val KEYSTORE_ASSET = "debug.keystore"
        private const val KEYSTORE_PASS = "bearite123"
        private const val KEY_ALIAS = "bearite"
    }

    private val work = File(ctx.filesDir, "patch_work")

    fun isGameInstalled(): Boolean = try {
        ctx.packageManager.getPackageInfo(GAME, 0); true
    } catch (e: Exception) { false }

    fun patch() {
        work.deleteRecursively()
        work.mkdirs()

        val info = ctx.packageManager.getPackageInfo(GAME, 0)
        val appInfo = info.applicationInfo ?: throw IllegalStateException("No app info for $GAME")
        val sourceApks = ArrayList<String>()
        sourceApks.add(appInfo.sourceDir)
        appInfo.splitSourceDirs?.let { sourceApks.addAll(it) }
        log("Файлов APK у игры: ${sourceApks.size}")

        var totalPatched = 0
        val rawApks = ArrayList<File>()
        for (src in sourceApks) {
            val srcFile = File(src)
            val outFile = File(work, srcFile.name)
            totalPatched += rezipWithPatch(srcFile, outFile)
            rawApks.add(outFile)
        }
        if (totalPatched == 0) {
            log("ВНИМАНИЕ: ни один метод защиты не изменён. Игра может уже запускаться без патча, или защита изменилась — пришли отчёт разработчику.")
        } else {
            log("Изменено методов защиты: $totalPatched")
        }

        val signerConfig = loadSigner()
        for (apk in rawApks) {
            val signedOut = File(work, apk.nameWithoutExtension + "-signed.apk")
            ApkSigner.Builder(listOf(signerConfig))
                .setInputApk(apk)
                .setOutputApk(signedOut)
                .setV1SigningEnabled(true)
                .setV2SigningEnabled(true)
                .setV3SigningEnabled(true)
                .setMinSdkVersion(26)
                .build()
                .sign()
        }
        log("Подпись готова.")
    }

    // Copies one APK entry by entry, patching every classes*.dex along the way.
    // Preserves each entry's original compression method: native libraries are
    // often stored uncompressed on purpose (extractNativeLibs=false expects
    // mmap access), and re-compressing them would break that.
    private fun rezipWithPatch(src: File, dst: File): Int {
        var patchedCount = 0
        ZipFile(src).use { zin ->
            ZipOutputStream(FileOutputStream(dst)).use { zout ->
                val entries = zin.entries().toList()
                for (e in entries) {
                    var bytes = zin.getInputStream(e).readBytes()
                    if (e.name.matches(Regex("classes[0-9]*\\.dex"))) {
                        val report = PairipPatch.patch(bytes)
                        val n = Regex("""PairIP: (\d+) method""").find(report)?.groupValues?.get(1)?.toIntOrNull() ?: 0
                        if (n > 0) {
                            patchedCount += n
                            log(report.trimEnd())
                        }
                    }
                    val ne = ZipEntry(e.name)
                    if (e.method == ZipEntry.STORED) {
                        ne.method = ZipEntry.STORED
                        ne.size = bytes.size.toLong()
                        ne.compressedSize = bytes.size.toLong()
                        ne.crc = crc32(bytes)
                    } else {
                        ne.method = ZipEntry.DEFLATED
                    }
                    zout.putNextEntry(ne)
                    zout.write(bytes)
                    zout.closeEntry()
                }
            }
        }
        return patchedCount
    }

    private fun loadSigner(): ApkSigner.SignerConfig {
        val ks = KeyStore.getInstance("PKCS12")
        ctx.assets.open(KEYSTORE_ASSET).use { ks.load(it, KEYSTORE_PASS.toCharArray()) }
        val key = ks.getKey(KEY_ALIAS, KEYSTORE_PASS.toCharArray()) as PrivateKey
        val chain = ks.getCertificateChain(KEY_ALIAS).map { it as X509Certificate }
        return ApkSigner.SignerConfig.Builder("bearite", key, chain).build()
    }

    fun install() {
        val toInstall = work.listFiles { f -> f.name.endsWith("-signed.apk") }?.toList().orEmpty()
        if (toInstall.isEmpty()) throw IllegalStateException("Нет подписанных файлов, сначала нажми Patch.")

        val installer = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        val sessionId = installer.createSession(params)
        val session = installer.openSession(sessionId)
        session.use {
            for (apk in toInstall) {
                session.openWrite(apk.name, 0, apk.length()).use { out ->
                    FileInputStream(apk).use { input -> input.copyTo(out) }
                    session.fsync(out)
                }
            }
            val intent = Intent(ACTION_INSTALL_RESULT).setPackage(ctx.packageName)
            val pi = PendingIntent.getBroadcast(
                ctx, 0, intent,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE
            )
            session.commit(pi.intentSender)
        }
        log("Установка отправлена, подтверди системный диалог, если он появится.")
    }

    fun cleanup() {
        work.deleteRecursively()
    }
}
