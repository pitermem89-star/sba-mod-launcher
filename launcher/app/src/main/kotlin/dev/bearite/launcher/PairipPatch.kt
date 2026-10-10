package dev.bearite.launcher

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.zip.Adler32

object PairipPatch {

    private const val PREFIX = "Lcom/pairip/"
    private const val CLIENT_HINT = "Lcom/pairip/licensecheck/LicenseClient;"

    private val KNOWN_NAMES = setOf(
        "initializeLicenseCheck", "checkLicense", "checkLicenseInternal",
        "bindToLicensingService", "handleError", "startErrorDialogActivity",
        "startPaywallActivity", "scheduleAppShutdown", "scheduleRepeatedLicenseCheck"
    )

    // Testing in isolation: StartupLauncher.launch() removed from this set for
    // now — the game crashed on launch after it was neutered, so we're
    // checking whether verifyIntegrity alone is safe before reintroducing it.
    private val EXPLICIT_TARGETS = setOf(
        "Lcom/pairip/SignatureCheck;" to "verifyIntegrity"
    )

    private val KILL_CALLS = setOf(
        "Ljava/lang/System;->exit",
        "Ljava/lang/Runtime;->exit",
        "Landroid/os/Process;->killProcess",
        "Landroid/app/Activity;->finish",
        "Landroid/app/Activity;->finishAffinity",
        "Landroid/content/Context;->bindService",
        "Landroid/content/Context;->startActivity",
        "Landroid/app/Activity;->startActivity",
        "Landroid/app/Activity;->startActivityForResult"
    )

    private val LIFECYCLE_NAMES = setOf(
        "onCreate", "onStart", "onRestart", "onResume",
        "onPause", "onStop", "onDestroy", "<init>", "<clinit>"
    )

    fun scan(dex: ByteArray): String {
        val reader = DexReader(dex)
        val hits = findHits(reader)
        val sb = StringBuilder("PairIP: ${hits.size} method(s) detected\n")
        for ((key, reason) in hits) sb.append("  - $key ($reason)\n")
        if (hits.isEmpty()) {
            sb.append("  Ничего не найдено в этом dex-файле.\n")
        }
        return sb.toString()
    }

    fun neuterInPlace(dex: ByteArray): Int {
        val reader = DexReader(dex)
        val hits = findHits(reader)
        var count = 0
        for (key in hits.keys) {
            val (cls, name) = key.split("->", limit = 2)
            val m = (reader.methods(cls) + reader.methodsByPrefix(PREFIX) + reader.methodsByPrefix("L"))
                .firstOrNull { it.cls == cls && it.name == name && it.shorty.startsWith("V") }
            if (m == null || m.codeOff == 0 || m.insnsSize < 1) continue
            val start = m.codeOff + 16
            val bytes = m.insnsSize * 2
            dex[start] = 0x0e.toByte()      // return-void
            dex[start + 1] = 0
            for (i in 2 until bytes) dex[start + i] = 0
            count++
        }
        if (count > 0) fixHeader(dex)
        return count
    }

    private fun findHits(reader: DexReader): LinkedHashMap<String, String> {
        val hits = LinkedHashMap<String, String>()

        for (m in reader.methods(CLIENT_HINT)) {
            if (m.name in KNOWN_NAMES && m.shorty.startsWith("V") && m.codeOff != 0) {
                hits["${m.cls}->${m.name}"] = "known name"
            }
        }

        for ((cls, name) in EXPLICIT_TARGETS) {
            for (m in reader.methods(cls)) {
                if (m.name == name && m.shorty.startsWith("V") && m.codeOff != 0) {
                    hits["${m.cls}->${m.name}"] = "explicit target"
                }
            }
        }

        for (m in reader.methodsByPrefix(PREFIX)) {
            if (m.name in LIFECYCLE_NAMES) continue
            val key = "${m.cls}->${m.name}"
            if (key in hits || !m.shorty.startsWith("V") || m.codeOff == 0) continue
            val call = reader.invokedSignatures(m).firstOrNull { sig -> KILL_CALLS.any { sig.startsWith(it) } }
            if (call != null) hits[key] = "calls $call"
        }

        return hits
    }

    private fun fixHeader(dex: ByteArray) {
        val md = MessageDigest.getInstance("SHA-1")
        md.update(dex, 32, dex.size - 32)
        System.arraycopy(md.digest(), 0, dex, 12, 20)
        val adler = Adler32()
        adler.update(dex, 12, dex.size - 12)
        val sum = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(adler.value.toInt()).array()
        System.arraycopy(sum, 0, dex, 8, 4)
    }
}
