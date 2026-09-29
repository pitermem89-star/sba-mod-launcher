package dev.bearite.launcher

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import com.android.apksig.ApkSigner
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.cert.X509Certificate
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import javax.security.auth.x500.X500Principal

class Patcher(private val ctx: Context, private val log: (String) -> Unit) {

    companion object {
        const val GAME = "com.Earthkwak.Platformer"
        const val ACTION_INSTALL_RESULT = "dev.bearite.launcher.INSTALL_RESULT"
        private const val LIB_MAIN = "lib/arm64-v8a/libmain.so"
        private const val LIB_MAIN_REAL = "lib/arm64-v8a/libmain_real.so"
        private const val LIB_BEARITE = "lib/arm64-v8a/libbearite.so"
        private const val KEY_ALIAS = "bearite-signing"
    }

    private val workDir = File(ctx.filesDir, "patched")
    private val readyMarker = File(workDir, "READY")

    fun isGameInstalled(): Boolean {
        return try {
            ctx.packageManager.getPackageInfo(GAME, 0)
            true
        } catch (e: Exception) {
            false
        }
    }

    fun preparedFiles(): List<File> {
        if (!readyMarker.exists()) return emptyList()
        val files = workDir.listFiles() ?: return emptyList()
        return files.filter { it.name.endsWith(".apk") }.sortedBy { it.name }
    }

    private fun sourceApks(): List<File> {
        val info = ctx.packageManager.getPackageInfo(GAME, 0)
        val app = info.applicationInfo ?: throw IllegalStateException("No app info")
        val list = ArrayList<File>()
        list.add(File(app.sourceDir))
        app.splitSourceDirs?.forEach { list.add(File(it)) }
        return list
    }

    private fun hasEntry(file: File, name: String): Boolean {
        val zip = ZipFile(file)
        try {
            return zip.getEntry(name) != null
        } finally {
            zip.close()
        }
    }

    fun patch() {
        val sources = sourceApks()
        for (s in sources) {
            if (hasEntry(s, LIB_MAIN_REAL)) {
                throw IllegalStateException(
                    "The installed game is already patched. Uninstall it, install the original from Google Play, then patch."
                )
            }
        }
        workDir.deleteRecursively()
        workDir.mkdirs()

        log("Preparing signing key...")
        val key = loadKey()
        val cert = loadCert()

        for (src in sources) {
            var input = src
            if (hasEntry(src, LIB_MAIN)) {
                log("Patching native libs in " + src.name + " ...")
                input = File(workDir, "tmp_" + src.name)
                rewriteNativeSplit(src, input)
            } else if (hasEntry(src, "classes.dex")) {
                val tmp = File(workDir, "tmp_" + src.name)
                log("Copying " + src.name + " ...")
                src.copyTo(tmp, true)
                log("Removing license check in " + src.name + " ...")
                val n = PairipPatch.patch(tmp)
                log("Neutralized " + n + " methods in " + src.name)
                if (n > 0) {
                    input = tmp
                } else {
                    tmp.delete()
                }
            }
            log("Signing " + src.name + " ...")
            val out = File(workDir, src.name)
            sign(input, out, key, cert)
            if (input != src) input.delete()
        }
        readyMarker.writeText("ok")
        log("Patched " + sources.size + " files.")
    }

    private fun rewriteNativeSplit(src: File, dst: File) {
        val zip = ZipFile(src)
        val zos = ZipOutputStream(BufferedOutputStream(FileOutputStream(dst)))
        try {
            zos.setLevel(1)
            val entries = zip.entries()
            while (entries.hasMoreElements()) {
                val e = entries.nextElement()
                if (e.name.startsWith("META-INF/")) continue
                val name = if (e.name == LIB_MAIN) LIB_MAIN_REAL else e.name
                zos.putNextEntry(ZipEntry(name))
                if (!e.isDirectory) {
                    zip.getInputStream(e).use { it.copyTo(zos) }
                }
                zos.closeEntry()
            }
            addAsset(zos, "libmain.so", LIB_MAIN)
            addAsset(zos, "libbearite.so", LIB_BEARITE)
        } finally {
            zos.close()
            zip.close()
        }
    }

    private fun addAsset(zos: ZipOutputStream, asset: String, entryName: String) {
        zos.putNextEntry(ZipEntry(entryName))
        ctx.assets.open(asset).use { it.copyTo(zos) }
        zos.closeEntry()
    }

    private fun ensureKey() {
        val ks = KeyStore.getInstance("AndroidKeyStore")
        ks.load(null)
        if (ks.containsAlias(KEY_ALIAS)) return
        val gen = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore")
        val spec = KeyGenParameterSpec.Builder(KEY_ALIAS, KeyProperties.PURPOSE_SIGN)
            .setDigests(KeyProperties.DIGEST_SHA256)
            .setSignaturePaddings(KeyProperties.SIGNATURE_PADDING_RSA_PKCS1)
            .setKeySize(2048)
            .setCertificateSubject(X500Principal("CN=Bearite"))
            .build()
        gen.initialize(spec)
        gen.generateKeyPair()
    }

    private fun loadKey(): PrivateKey {
        ensureKey()
        val ks = KeyStore.getInstance("AndroidKeyStore")
        ks.load(null)
        return ks.getKey(KEY_ALIAS, null) as PrivateKey
    }

    private fun loadCert(): X509Certificate {
        val ks = KeyStore.getInstance("AndroidKeyStore")
        ks.load(null)
        return ks.getCertificate(KEY_ALIAS) as X509Certificate
    }

    private fun sign(input: File, output: File, key: PrivateKey, cert: X509Certificate) {
        val config = ApkSigner.SignerConfig.Builder("bearite", key, listOf(cert)).build()
        ApkSigner.Builder(listOf(config))
            .setInputApk(input)
            .setOutputApk(output)
            .setMinSdkVersion(24)
            .setV1SigningEnabled(false)
            .setV2SigningEnabled(true)
            .setV3SigningEnabled(false)
            .build()
            .sign()
    }

    fun install() {
        val files = preparedFiles()
        if (files.isEmpty()) throw IllegalStateException("No prepared files.")
        val installer = ctx.packageManager.packageInstaller
        val params = PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL)
        params.setAppPackageName(GAME)
        val id = installer.createSession(params)
        val session = installer.openSession(id)
        try {
            for (f in files) {
                log("Writing " + f.name + " ...")
                FileInputStream(f).use { input ->
                    val out = session.openWrite(f.name, 0, f.length())
                    try {
                        input.copyTo(out)
                        session.fsync(out)
                    } finally {
                        out.close()
                    }
                }
            }
            val intent = Intent(ACTION_INSTALL_RESULT).setPackage(ctx.packageName)
            var flags = PendingIntent.FLAG_UPDATE_CURRENT
            if (Build.VERSION.SDK_INT >= 31) flags = flags or PendingIntent.FLAG_MUTABLE
            val pending = PendingIntent.getBroadcast(ctx, id, intent, flags)
            session.commit(pending.intentSender)
        } finally {
            session.close()
        }
    }

    fun cleanup() {
        workDir.deleteRecursively()
    }
}
