package com.example.esp32_demo

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import kotlin.math.max
import kotlin.math.min

class HistoryGraphView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null, defStyle: Int = 0
) : View(context, attrs, defStyle) {

    private data class Sample(val fps: Float, val minC: Float, val maxC: Float, val avgC: Float)

    private val samples = ArrayList<Sample>(MAX_SAMPLES)
    private val linePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#4FC3F7")
        strokeWidth = 3f
        style = Paint.Style.STROKE
    }
    private val tempPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#FFB74D")
        strokeWidth = 3f
        style = Paint.Style.STROKE
    }
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#334553")
        strokeWidth = 1f
    }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#B0BEC5")
        textSize = 24f
    }

    fun addSample(fps: Double, minC: Double, maxC: Double, avgC: Double) {
        if (samples.size == MAX_SAMPLES) samples.removeAt(0)
        samples.add(Sample(fps.toFloat(), minC.toFloat(), maxC.toFloat(), avgC.toFloat()))
        invalidate()
    }

    fun clear() {
        samples.clear()
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        super.onDraw(canvas)
        val left = paddingLeft + 10f
        val right = width - paddingRight - 10f
        val top = paddingTop + 28f
        val bottom = height - paddingBottom - 14f
        val graphWidth = (right - left).coerceAtLeast(1f)
        val graphHeight = (bottom - top).coerceAtLeast(1f)

        canvas.drawText("趋势  FPS / 平均温", left, paddingTop + 20f, textPaint)
        for (i in 0..3) {
            val y = top + graphHeight * i / 3f
            canvas.drawLine(left, y, right, y, gridPaint)
        }
        if (samples.size < 2) return

        var minTemp = Float.MAX_VALUE
        var maxTemp = -Float.MAX_VALUE
        var maxFps = 1f
        for (s in samples) {
            minTemp = min(minTemp, s.minC)
            maxTemp = max(maxTemp, s.maxC)
            maxFps = max(maxFps, s.fps)
        }
        val tempRange = (maxTemp - minTemp).coerceAtLeast(1f)
        val step = graphWidth / (MAX_SAMPLES - 1)

        var prevX = left
        var prevFpsY = bottom - samples[0].fps / maxFps * graphHeight
        var prevTempY = bottom - (samples[0].avgC - minTemp) / tempRange * graphHeight

        for (i in 1 until samples.size) {
            val x = left + i * step
            val fpsY = bottom - samples[i].fps / maxFps * graphHeight
            val tempY = bottom - (samples[i].avgC - minTemp) / tempRange * graphHeight
            canvas.drawLine(prevX, prevFpsY, x, fpsY, linePaint)
            canvas.drawLine(prevX, prevTempY, x, tempY, tempPaint)
            prevX = x
            prevFpsY = fpsY
            prevTempY = tempY
        }
    }

    companion object {
        private const val MAX_SAMPLES = 90
    }
}
