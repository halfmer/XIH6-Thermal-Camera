package com.example.esp32_demo

import kotlin.math.abs

/**
 * 撕裂门禁，移植自 Qt 端 framegate.cpp 的思路：
 * Lepton 3.5 每帧由 4 个 VoSPI 段（各 30 行）拼成，接缝在行 29|30、59|60、89|90。
 * 若某接缝处的行间差异远超全帧中位差异，说明相邻段来自不同采集时刻 → 判为撕裂。
 * 连续 kForceStreak 次撕裂则强制放行一次，避免真实热边缘卡死画面。
 */
class FrameGate {
    private var tornStreak = 0

    /** display=是否允许刷新显示；torn=本帧是否判定为撕裂 */
    data class Result(val display: Boolean, val torn: Boolean)

    fun accept(px: IntArray, width: Int, height: Int): Result {
        if (height < 4) { tornStreak = 0; return Result(true, false) }

        val rowDiff = DoubleArray(height)
        for (r in 1 until height) {
            var s = 0L
            val base = r * width
            val prev = (r - 1) * width
            for (c in 0 until width) s += abs(px[base + c] - px[prev + c]).toLong()
            rowDiff[r] = s.toDouble() / width
        }
        val sorted = rowDiff.copyOfRange(1, height).sortedArray()
        val median = sorted[sorted.size / 2].coerceAtLeast(1e-6)

        var torn = false
        for (seam in intArrayOf(30, 60, 90)) {
            if (seam >= height) continue
            val d = rowDiff[seam]
            if (d > K_TEAR_RATIO * median && d > K_TEAR_MIN_ABS) { torn = true; break }
        }

        return if (torn) {
            tornStreak++
            if (tornStreak >= K_FORCE_STREAK) { tornStreak = 0; Result(true, true) }
            else Result(false, true)
        } else {
            tornStreak = 0
            Result(true, false)
        }
    }

    companion object {
        private const val K_TEAR_RATIO = 8.0
        private const val K_TEAR_MIN_ABS = 80.0
        private const val K_FORCE_STREAK = 5
    }
}
