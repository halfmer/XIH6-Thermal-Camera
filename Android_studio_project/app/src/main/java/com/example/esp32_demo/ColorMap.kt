package com.example.esp32_demo

/**
 * 伪彩映射：原生 16-bit 原始值 → ARGB。移植自 Qt 端 colormap.cpp。
 * 每帧自动拉伸对比度（min..max → 0..255）后查 256 级调色板 LUT。
 * 温度模型：Lepton 3.5 原始值为厘开尔文，摄氏 = raw*0.01 - 273.15。
 */
object ColorMap {

    enum class Palette(val label: String) { IRONBOW("铁虹"), WHITEHOT("白热"), RAINBOW("彩虹") }

    class Rendered(val argb: IntArray, val minC: Double, val maxC: Double, val avgC: Double)

    private val luts = HashMap<Palette, IntArray>()

    fun rawToCelsius(raw: Int): Double = raw * 0.01 - 273.15

    fun render(px: IntArray, palette: Palette): Rendered {
        var vmin = Int.MAX_VALUE
        var vmax = Int.MIN_VALUE
        var sum = 0L
        for (v in px) {
            if (v < vmin) vmin = v
            if (v > vmax) vmax = v
            sum += v
        }
        val n = px.size
        val range = (vmax - vmin).coerceAtLeast(1)
        val scale = 255.0 / range
        val lut = lutOf(palette)
        val argb = IntArray(n)
        for (i in 0 until n) {
            var idx = ((px[i] - vmin) * scale).toInt()
            if (idx < 0) idx = 0 else if (idx > 255) idx = 255
            argb[i] = lut[idx]
        }
        val avgRaw = (sum / n).toInt()
        return Rendered(argb, rawToCelsius(vmin), rawToCelsius(vmax), rawToCelsius(avgRaw))
    }

    private fun lutOf(p: Palette): IntArray = luts.getOrPut(p) {
        when (p) {
            Palette.IRONBOW -> gradient(arrayOf(
                intArrayOf(0, 0, 0, 0), intArrayOf(32, 20, 0, 60), intArrayOf(80, 120, 0, 120),
                intArrayOf(128, 200, 30, 60), intArrayOf(170, 255, 90, 0), intArrayOf(210, 255, 180, 0),
                intArrayOf(240, 255, 240, 120), intArrayOf(255, 255, 255, 255)
            ))
            Palette.WHITEHOT -> gradient(arrayOf(intArrayOf(0, 0, 0, 0), intArrayOf(255, 255, 255, 255)))
            Palette.RAINBOW -> gradient(arrayOf(
                intArrayOf(0, 0, 0, 255), intArrayOf(64, 0, 255, 255), intArrayOf(128, 0, 255, 0),
                intArrayOf(192, 255, 255, 0), intArrayOf(255, 255, 0, 0)
            ))
        }
    }

    /** stops 每项 = [位置0..255, r, g, b]，线性插值成 256 级 LUT。 */
    private fun gradient(stops: Array<IntArray>): IntArray {
        val lut = IntArray(256)
        for (i in 0..255) {
            var a = stops[0]
            var b = stops[stops.size - 1]
            for (s in 0 until stops.size - 1) {
                if (i >= stops[s][0] && i <= stops[s + 1][0]) { a = stops[s]; b = stops[s + 1]; break }
            }
            val span = (b[0] - a[0]).coerceAtLeast(1)
            val t = (i - a[0]).toDouble() / span
            val r = (a[1] + (b[1] - a[1]) * t).toInt()
            val g = (a[2] + (b[2] - a[2]) * t).toInt()
            val bl = (a[3] + (b[3] - a[3]) * t).toInt()
            lut[i] = (0xFF shl 24) or (r shl 16) or (g shl 8) or bl
        }
        return lut
    }
}
