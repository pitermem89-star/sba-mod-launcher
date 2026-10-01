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

    private fun typeName(idx: Int): String {
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

    private fun methodInfo(cls: String, idx: Int, codeOff: Int): MethodInfo {
        val m = methodIdsOff + idx * 8
        val protoIdx = buf.getShort(m + 2).toInt() and 0xffff
        val nameIdx = buf.getInt(m + 4)
        val p = protoIdsOff + protoIdx * 12
        val shorty = string(buf.getInt(p))
        val size = if (codeOff != 0) buf.getInt(codeOff + 12) else 0
        return MethodInfo(cls, string(nameIdx), shorty, codeOff, size)
    }

    fun methods(className: String): List<MethodInfo> {
        val result = ArrayList<MethodInfo>()
        for (i in 0 until classDefsSize) {
            val def = classDefsOff + i * 32
            val cname = typeName(buf.getInt(def))
            if (cname != className) continue
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
                    uleb(pos)
                    val codeOff = uleb(pos)
                    result.add(methodInfo(cname, methodIdx, codeOff))
                }
            }
        }
        return result
    }
}

object PairipScan {

    fun run(ctx: Context): String {
        return try {
            val info = ctx.packageManager.getPackageInfo(Patcher.GAME, 0)
            val app = info.applicationInfo ?: return "Ошибка: Данные оригинального приложения не найдены."
            val zip = ZipFile(File(app.sourceDir))
            try {
                val entry = zip.getEntry("classes.dex") ?: return "Внимание: Файл classes.dex не найден в APK."
                val bytes = zip.getInputStream(entry).use { it.readBytes() }
                val sb = StringBuilder()
                sb.append("Оригинальный classes.dex: ").append(bytes.size / 1024).append(" KB\n")
                
                val list = DexReader(bytes).methods(PairipPatch.CLIENT)
                var n = 0
                for (m in list) {
                    if (m.name in PairipPatch.TARGETS && m.shorty.startsWith("V") && m.codeOff != 0) {
                        sb.append("Обнаружен целевой метод защиты: ").append(m.name).append(' ')
                        sb.append(m.shorty).append(" (размер блоков: ").append(m.insnsSize).append(")\n")
                        n++
                    }
                }
                sb.append("Сканирование завершено успешно. Обнаружено методов: ").append(n).append('\n')
                sb.toString()
            } finally {
                zip.close()
            }
        } catch (e: Exception) {
            "Ошибка при анализе оригинального APK: ${e.message}"
        }
    }
}
