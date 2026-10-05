package dev.bearite.launcher

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.util.zip.Adler32
import java.util.zip.CRC32

object PairipPatch {

    private const val PREFIX = "Lcom/pairip/"
    private const val CLIENT_HINT = "Lcom/pairip/licensecheck/LicenseClient;"

    // Exact names seen in builds up to 13.0.5. Checked first so a clean match
    // is still reported precisely even though the scan below would find it too.
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

    private fun le(b: ByteArray): ByteBuffer = ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN)

    // Patches classes.dex inside the (stored) APK in place. Returns number of methods changed.
    fun patch(apk: File): Int {
        val raf = RandomAccessFile(apk, "rw")
        try {
            return patchOpen(raf)
        } finally {
            raf.close()
        }
    }

    private fun patchOpen(raf: RandomAccessFile): Int {
        val len = raf.length()
        val tailLen = minOf(len, 70000L).toInt()
        val tail = ByteArray(tailLen)
        raf.seek(len - tailLen)
        raf.readFully(tail)
        var eocd = -1
        for (i in tailLen - 22 downTo 0) {
            if (tail[i] == 0x50.toByte() && tail[i + 1] == 0x4b.toByte() &&
                tail[i + 2] == 0x05.toByte() && tail[i + 3] == 0x06.toByte()
            ) {
                eocd = i
                break
            }
        }
        if (eocd < 0) throw IllegalStateException("No zip end record")
        val tb = le(tail)
        val cdSize = tb.getInt(eocd + 12)
        val cdOffset = tb.getInt(eocd + 16).toLong() and 0xffffffffL
        val cd = ByteArray(cdSize)
        raf.seek(cdOffset)
        raf.readFully(cd)
        val cb = le(cd)

        var p = 0
        var found = -1
        while (p + 46 <= cdSize && cb.getInt(p) == 0x02014b50) {
            val nameLen = cb.getShort(p + 28).toInt() and 0xffff
            val extraLen = cb.getShort(p + 30).toInt() and 0xffff
            val commentLen = cb.getShort(p + 32).toInt() and 0xffff
            val name = String(cd, p + 46, nameLen, Charsets.UTF_8)
            if (name == "classes.dex") {
                found = p
                break
            }
            p += 46 + nameLen + extraLen + commentLen
        }
        if (found < 0) return 0

        val flags = cb.getShort(found + 8).toInt() and 0xffff
        val method = cb.getShort(found + 10).toInt() and 0xffff
        val usize = cb.getInt(found + 24)
        val lfh = cb.getInt(found + 42).toLong() and 0xffffffffL
        if (method != 0 || (flags and 8) != 0) {
            throw IllegalStateException("classes.dex is compressed, cannot patch in place")
        }

        val header = ByteArray(30)
        raf.seek(lfh)
        raf.readFully(header)
        val hb = le(header)
        val nameLen = hb.getShort(26).toInt() and 0xffff
        val extraLen = hb.getShort(28).toInt() and 0xffff
        val dataOff = lfh + 30 + nameLen + extraLen

        val dex = ByteArray(usize)
        raf.seek(dataOff)
        raf.readFully(dex)
        val count = neuter(dex)
        if (count == 0) return 0
        fixHeader(dex)

        raf.seek(dataOff)
        raf.write(dex)
        val crc = CRC32()
        crc.update(dex)
        val crcBytes = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(crc.value.toInt()).array()
        raf.seek(lfh + 14)
        raf.write(crcBytes)
        raf.seek(cdOffset + found + 16)
        raf.write(crcBytes)
        return count
    }

    // Read-only variant for a "Diagnose game" button: same detection, no writes.
    fun scan(dex: ByteArray): String {
        val reader = DexReader(dex)
        val hits = findHits(reader)
        val sb = StringBuilder("PairIP: ${hits.size} method(s) detected\n")
        for ((key, reason) in hits) sb.append("  - $key ($reason)\n")
        if (hits.isEmpty()) sb.append("  Ничего не найдено — защита не распознана этим сканером.\n")
        return sb.toString()
    }

    private fun findHits(reader: DexReader): LinkedHashMap<String, String> {
        val hits = LinkedHashMap<String, String>()
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
        return hits
    }

    private fun neuter(dex: ByteArray): Int {
        val reader = DexReader(dex)
        val hits = findHits(reader)
        var count = 0
        for (key in hits.keys) {
            val (cls, name) = key.split("->", limit = 2)
            val m = (reader.methods(cls) + reader.methodsByPrefix(PREFIX))
                .firstOrNull { it.cls == cls && it.name == name && it.shorty == "V" }
            if (m == null || m.codeOff == 0 || m.insnsSize < 1) continue
            val start = m.codeOff + 16
            val bytes = m.insnsSize * 2
            dex[start] = 0x0e.toByte()      // return-void
            dex[start + 1] = 0
            for (i in 2 until bytes) dex[start + i] = 0  // nops
            count++
        }
        return count
    }

    private fun fixHeader(dex: ByteArray) {
        val md = MessageDigest.getInstance("SHA-1")
        md.update(dex, 32, dex.size - 32)
        System.arraycopy(md.digest(), 0, dex, 12, 20)
        val adler = Adler32()
        adler.update(dex, 12, dex.size - 12)
        val sum = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN)
            .putInt(adler.value.toInt()).array()
        System.arraycopy(sum, 0, dex, 8, 4)
    }
}
