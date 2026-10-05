package dev.bearite.launcher

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.zip.Adler32

object PairipPatch {

    private const val PREFIX = "Lcom/pairip/"
    private const val CLIENT_HINT = "Lcom/pairip/licensecheck/LicenseClient;"

    // Exact names seen in builds up to 13.0.5. Checked first so a clean match
    // still gets reported precisely even if the slower scan would find it too.
    private val KNOWN_NAMES = setOf(
        "initializeLicenseCheck", "checkLicense", "checkLicenseInternal",
        "bindToLicensingService", "handleError", "startErrorDialogActivity",
        "startPaywallActivity", "scheduleAppShutdown", "scheduleRepeatedLicenseCheck"
    )

    // A void, no-arg method under com/pairip/ that calls one of these is almost
    // certainly part of the shutdown/paywall path, whatever it ends up named.
    private val KILL_CALLS = setOf(
        "Ljava/lang/System;->exit",
        "Ljava/lang/Runtime;->exit",
        "Landroid/os/Process;->killProcess",
        "Landroid/app/Activity;->finish",
        "Landroid/app/Activity;->finishAffinity",
        "Landroid/content/Context;->bindService"
    )

    // Patches a dex file's bytes in place (same length; only method bodies change).
    fun patch(dex: ByteArray): String = run(dex, apply = true)

    // Same detection, read-only — used by the "Diagnose game" button.
    fun scan(dex: ByteArray): String = run(dex, apply = false)

    private fun run(dex: ByteArray, apply: Boolean): String {
        val reader = DexReader(dex)
        val hits = LinkedHashMap<String, String>()  // "class->name" -> reason

        for (m in reader.methods(CLIENT_HINT)) {
            if (m.name in KNOWN_NAMES && m.shorty == "V" && m.codeOff != 0) {
                hits["${m.cls}->${m.name}"] = "known name"
            }
        }
        for (m in reader.methodsByPrefix(PREFIX)) {
            val key = "${m.cls}->${m.name}"
            if (key in hits || m.shorty != "V" || m.codeOff == 0) continue
            val call = reader.invokedSignatures(m).firstOrNull { sig -> KILL_CALLS.any { sig.startsWith(it) } }
            if (call != null) hits[key] = "calls $call"
        }

        val report = StringBuilder()
        if (apply) {
            var changed = false
            for ((key, _) in hits) {
                val (cls, name) = key.split("->", limit = 2)
                val m = (reader.methods(cls) + reader.methodsByPrefix(PREFIX))
                    .firstOrNull { it.cls == cls && it.name == name && it.shorty == "V" }
                if (m != null && neuter(dex, m)) changed = true
            }
            if (changed) fixHeader(dex)
        }

        report.append("PairIP: ").append(hits.size).append(" method(s) ")
            .append(if (apply) "patched" else "detected").append('\n')
        for ((key, reason) in hits) report.append("  - ").append(key).append(" (").append(reason).append(")\n")
        if (hits.isEmpty()) {
            report.append("  Ничего не найдено. Игра может быть не защищена, уже пропатчена, ")
                .append("или защита изменилась так, что этот сканер её не узнаёт.\n")
        }
        return report.toString()
    }

    private fun neuter(dex: ByteArray, m: MethodInfo): Boolean {
        if (m.insnsSize < 1) return false
        val start = m.codeOff + 16
        dex[start] = 0x0e.toByte()      // return-void
        dex[start + 1] = 0
        val bytes = m.insnsSize * 2
        for (i in 2 until bytes) dex[start + i] = 0
        return true
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
