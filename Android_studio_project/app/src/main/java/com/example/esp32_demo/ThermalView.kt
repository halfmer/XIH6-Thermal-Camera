package com.example.esp32_demo

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Rect
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import kotlin.math.min

/**
 * 热成像显示控件。只画"已完整接收并渲染好"的 Bitmap（双缓冲的前缓冲）。
 * 保持 4:3 宽高比缩放居中；叠加帧号/FPS/最高·最低·平均温；点按取点温。
 */
class ThermalView @JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null, defStyle: Int = 0
) : View(context, attrs, defStyle) {

    private var bitmap: Bitmap? = null
    private var frame: ThermalFrame? = null
    private var minC = 0.0
    private var maxC = 0.0
    private var avgC = 0.0
    private var fps = 0.0
    private var frameId = 0
    private var showOverlay = true
    private var showGrid = false
    private var spotEnabled = true

    private var spotX = -1  // 图像像素坐标
    private var spotY = -1
    private val dst = Rect()

    private val bmpPaint = Paint(Paint.FILTER_BITMAP_FLAG).apply { isFilterBitmap = false }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; textSize = 34f; setShadowLayer(4f, 0f, 0f, Color.BLACK)
    }
    private val hintPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#B0BEC5"); textSize = 40f; textAlign = Paint.Align.CENTER
    }
    private val crossPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.WHITE; strokeWidth = 2f; setShadowLayer(3f, 0f, 0f, Color.BLACK)
    }
    private val gridPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.argb(110, 255, 255, 255); strokeWidth = 1.5f
    }

    fun show(bmp: Bitmap, f: ThermalFrame, minC: Double, maxC: Double, avgC: Double, fps: Double) {
        this.bitmap = bmp
        this.frame = f
        this.minC = minC; this.maxC = maxC; this.avgC = avgC; this.fps = fps
        this.frameId = f.frameId
        invalidate()
    }

    fun setOverlayEnabled(enabled: Boolean) {
        showOverlay = enabled
        invalidate()
    }

    fun setGridEnabled(enabled: Boolean) {
        showGrid = enabled
        invalidate()
    }

    fun setSpotEnabled(enabled: Boolean) {
        spotEnabled = enabled
        if (!enabled) {
            spotX = -1
            spotY = -1
        }
        invalidate()
    }

    fun clear() {
        bitmap = null; frame = null; spotX = -1; spotY = -1
        invalidate()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (spotEnabled && event.action == MotionEvent.ACTION_DOWN && bitmap != null && !dst.isEmpty) {
            val f = frame ?: return true
            val ix = ((event.x - dst.left) / dst.width() * f.width).toInt()
            val iy = ((event.y - dst.top) / dst.height() * f.height).toInt()
            if (ix in 0 until f.width && iy in 0 until f.height) {
                spotX = ix; spotY = iy; invalidate()
            }
            return true
        }
        return super.onTouchEvent(event)
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.parseColor("#0B0F14"))
        val bmp = bitmap
        if (bmp == null) {
            canvas.drawText("等待 ESP32 接入…", width / 2f, height / 2f, hintPaint)
            return
        }
        // 保持宽高比缩放居中
        val scale = min(width.toFloat() / bmp.width, height.toFloat() / bmp.height)
        val w = (bmp.width * scale).toInt()
        val h = (bmp.height * scale).toInt()
        val left = (width - w) / 2
        val top = (height - h) / 2
        dst.set(left, top, left + w, top + h)
        canvas.drawBitmap(bmp, null, dst, bmpPaint)

        if (showGrid) {
            for (i in 1 until 4) {
                val x = dst.left + dst.width() * i / 4f
                canvas.drawLine(x, dst.top.toFloat(), x, dst.bottom.toFloat(), gridPaint)
            }
            for (i in 1 until 3) {
                val y = dst.top + dst.height() * i / 3f
                canvas.drawLine(dst.left.toFloat(), y, dst.right.toFloat(), y, gridPaint)
            }
        }

        // 叠加信息
        if (showOverlay) {
            var y = 40f
            canvas.drawText("帧 #$frameId   FPS ${"%.1f".format(fps)}", 16f, y, textPaint)
            y += 40f
            canvas.drawText(
                "最高 ${"%.1f".format(maxC)}°  最低 ${"%.1f".format(minC)}°  平均 ${"%.1f".format(avgC)}°",
                16f, y, textPaint
            )
        }

        // 点温
        val f = frame
        if (spotEnabled && f != null && spotX >= 0 && spotY >= 0) {
            val raw = f.pixels[spotY * f.width + spotX]
            val c = ColorMap.rawToCelsius(raw)
            val cx = dst.left + (spotX + 0.5f) / f.width * dst.width()
            val cy = dst.top + (spotY + 0.5f) / f.height * dst.height()
            canvas.drawLine(cx - 20, cy, cx + 20, cy, crossPaint)
            canvas.drawLine(cx, cy - 20, cx, cy + 20, crossPaint)
            canvas.drawText("点温 ${"%.1f".format(c)}°C", cx + 24, cy - 10, textPaint)
        }
    }
}
