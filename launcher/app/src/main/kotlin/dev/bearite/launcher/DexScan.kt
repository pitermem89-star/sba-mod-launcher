package dev.bearite.launcher

import android.content.Context
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.zip.ZipFile

class MethodInfo(
    val cls: String,
    val name: String,
    val shorty: String,
    val codeOff: Int,
    val insnsSize: Int
)

class DexReader(private val data: ByteArray) {

    private val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)
    private val stringIdsOff = buf.getInt(60)
    private val typeIdsOff = buf.getInt(68)
    private val protoIdsOff = buf.getInt(76)
    private val methodIdsOff = buf.getInt(92)
    private val classDefsSize = buf.getInt(96)
    private val classDefsOff = buf.getInt(100)

    private fun string(idx: Int): String {
        var p = buf.getInt(stringIdsOff + idx * 4)
        while ((data[p].toInt() and 0x80) != 0) p++
        p++
        val start = p
        while (data[p].toInt() != 0) p++
        return String(data, start, p - start, Charsets.UTF_8)
    }

    private fun typeName(idx: Int): String = string(buf.getInt(typeIdsOff + idx * 4))

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

    // "Lclass;->name" for a method_ids index — used to see what a call targets.
    fun methodSignature(methodIdx: Int): String {
        val m = methodIdsOff + methodIdx * 8
        val classIdx = buf.getShort(m).toInt() and 0xffff
        val nameIdx = buf.getInt(m + 4)
        return typeName(classIdx) + "->" + string(nameIdx)
    }

    private fun methodInfo(cls: String, idx: Int, codeOff: Int): MethodInfo {
        val m = methodIdsOff + idx * 8
        val protoIdx = buf.getShort(m + 2).toInt() and 0xffff
        val nameIdx = buf.getInt(m + 4)
        val p = protoIdsOff + protoIdx * 12
        val shorty = string(buf.getInt(p))
        val size = if (codeOff != 0) buf.getInt(codeOff + 12) else 0
        return MethodInfo(cls, string(nameIdx), shorty, codeOff, size)
    }

    private fun collectClassMethods(def: Int, cname: String, out: MutableList<MethodInfo>) {
        val dataOff = buf.getInt(def + 24)
        if (dataOff == 0) return
        val pos = intArrayOf(dataOff)
        val staticFields = uleb(pos)
        val instanceFields = uleb(pos)
        val directMethods = uleb(pos)
        val virtualMethods = uleb(pos)
        for (k in 0 until (staticFields + instanceFields)) { uleb(pos); uleb(pos) }
        for (list in 0..1) {
            var methodIdx = 0
            val count = if (list == 0) directMethods else virtualMethods
            for (k in 0 until count) {
                methodIdx += uleb(pos)
                uleb(pos)
                val codeOff = uleb(pos)
                out.add(methodInfo(cname, methodIdx, codeOff))
            }
        }
    }

    // Original behaviour: methods of one exact class name.
    fun methods(className: String): List<MethodInfo> {
        val result = ArrayList<MethodInfo>()
        for (i in 0 until classDefsSize) {
            val def = classDefsOff + i * 32
            if (typeName(buf.getInt(def)) == className) collectClassMethods(def, className, result)
        }
        return result
    }

    // New: every method of every class whose type name starts with `prefix`,
    // e.g. "Lcom/pairip/" — survives PairIP renaming the class itself.
    fun methodsByPrefix(prefix: String): List<MethodInfo> {
        val result = ArrayList<MethodInfo>()
        for (i in 0 until classDefsSize) {
            val def = classDefsOff + i * 32
            val cname = typeName(buf.getInt(def))
            if (cname.startsWith(prefix)) collectClassMethods(def, cname, result)
        }
        return result
    }

    // New: method signatures this method's bytecode calls. Lightweight scan —
    // looks only for invoke-* opcodes, doesn't decode every other opcode's
    // operand length precisely, so on rare byte patterns it can report one
    // extra signature. Used only as one signal among several, so a stray
    // false positive here is harmless.
    fun invokedSignatures(m: MethodInfo): Set<String> {
        if (m.codeOff == 0) return emptySet()
        val insnsOff = m.codeOff + 16
        val n = m.insnsSize
        val out = HashSet<String>()
        var i = 0
        while (i < n) {
            val lo = data[insnsOff + i * 2].toInt() and 0xff
            when (lo) {
                0x6e, 0x6f, 0x70, 0x71, 0x72,
                0x74, 0x75, 0x76, 0x77, 0x78 -> {
                    if (i + 2 < n) {
                        val methodIdx =
                            (data[insnsOff + (i + 1) * 2].toInt() and 0xff) or
                            ((data[insnsOff + (i + 1) * 2 + 1].toInt() and 0xff) shl 8)
                        out.add(methodSignature(methodIdx))
                    }
                    i += 3
                }
                else -> i += 1
            }
        }
        return out
    }
}

object PairipScan {
    fun run(ctx: Context): String = try {
        val info = ctx.packageManager.getPackageInfo(Patcher.GAME, 0)
        val app = info.applicationInfo ?: return "Ошибка: данные игры не найдены."
        val zip = ZipFile(File(app.sourceDir))
        try {
            val entry = zip.getEntry("classes.dex") ?: return "classes.dex не найден в APK."
            val bytes = zip.getInputStream(entry).use { it.readBytes() }
            "Оригинальный classes.dex: ${bytes.size / 1024} KB\n\n" + PairipPatch.scan(bytes)
        } finally {
            zip.close()
        }
    } catch (e: Exception) {
        "Ошибка при анализе APK: ${e.message}"
    }
}
