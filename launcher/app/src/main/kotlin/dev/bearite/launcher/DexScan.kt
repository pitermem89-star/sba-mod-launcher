package dev.bearite.launcher

import android.content.Context
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipFile

object PairipScan {

    fun run(ctx: Context): String {
        val info = ctx.packageManager.getPackageInfo(Patcher.GAME, 0)
        val app = info.applicationInfo ?: return "No app info."
        val sb = StringBuilder()
        val apks = ArrayList<File>()
        apks.add(File(app.sourceDir))
        app.splitSourceDirs?.forEach { apks.add(File(it)) }
        for (apk in apks) {
            val zip = ZipFile(apk)
            try {
                val entries = zip.entries()
                while (entries.hasMoreElements()) {
                    val e = entries.nextElement()
                    if (!e.name.startsWith("classes") || !e.name.endsWith(".dex")) continue
                    val bytes = zip.getInputStream(e).use { it.readBytes() }
                    sb.append("== ").append(apk.name).append(" / ").append(e.name).append('\n')
                    DexReader(bytes).dumpClasses("Lcom/pairip/", sb)
                }
            } finally {
                zip.close()
            }
        }
        return sb.toString()
    }
}

class DexReader(private val data: ByteArray) {

    private val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    private val stringIdsOff = buf.getInt(60)
    private val typeIdsOff = buf.getInt(68)
    private val protoIdsOff = buf.getInt(76)
    private val methodIdsOff = buf.getInt(92)
    private val classDefsSize = buf.getInt(96)
    private val classDefsOff = buf.getInt(100)

    fun string(idx: Int): String {
        var p = buf.getInt(stringIdsOff + idx * 4)
        while ((data[p].toInt() and 0x80) != 0) p++
        p++
        val start = p
        while (data[p].toInt() != 0) p++
        return String(data, start, p - start, Charsets.UTF_8)
    }

    fun typeName(idx: Int): String {
        return string(buf.getInt(typeIdsOff + idx * 4))
    }

    private fun uleb(pos: IntArray): Int {
        var result = 0
        var shift = 0
        while (true) {
            val b = data[pos[0]].toInt() and 0xff
            pos[0] = pos[0] + 1
            result = result or ((b and 0x7f) shl shift)
            if ((b and 0x80) == 0) break
            shift += 7
        }
        return result
    }

    fun dumpClasses(prefix: String, sb: StringBuilder) {
        for (i in 0 until classDefsSize) {
            val def = classDefsOff + i * 32
            val cname = typeName(buf.getInt(def))
            if (!cname.startsWith(prefix)) continue
            val superIdx = buf.getInt(def + 8)
            val superName = if (superIdx < 0) "-" else typeName(superIdx)
            sb.append(cname).append(" : ").append(superName).append('\n')
            val dataOff = buf.getInt(def + 24)
            if (dataOff == 0) continue
            val pos = intArrayOf(dataOff)
            val staticFields = uleb(pos)
            val instanceFields = uleb(pos)
            val directMethods = uleb(pos)
            val virtualMethods = uleb(pos)
            for (k in 0 until (staticFields + instanceFields)) {
                uleb(pos)
                uleb(pos)
            }
            for (list in 0..1) {
                var methodIdx = 0
                val count = if (list == 0) directMethods else virtualMethods
                for (k in 0 until count) {
                    methodIdx += uleb(pos)
                    val flags = uleb(pos)
                    val codeOff = uleb(pos)
                    sb.append(if (list == 0) "  D " else "  V ")
                    describeMethod(methodIdx, flags, codeOff, sb)
                }
            }
        }
    }

    private fun describeMethod(idx: Int, flags: Int, codeOff: Int, sb: StringBuilder) {
        val m = methodIdsOff + idx * 8
        val protoIdx = buf.getShort(m + 2).toInt() and 0xffff
        val nameIdx = buf.getInt(m + 4)
        val p = protoIdsOff + protoIdx * 12
        val shorty = string(buf.getInt(p))
        val ret = typeName(buf.getInt(p + 4))
        sb.append(string(nameIdx)).append(' ').append(shorty).append(' ').append(ret)
        if ((flags and 0x100) != 0) sb.append(" native")
        if (codeOff != 0) sb.append(" code=").append(buf.getInt(codeOff + 12))
        sb.append('\n')
    }
}
