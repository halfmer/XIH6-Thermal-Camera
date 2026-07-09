package com.example.esp32_demo

import android.graphics.Bitmap
import android.os.Handler
import android.os.Looper
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

/** 运行统计（每帧回调时拷贝一份给 UI）。 */
data class Stats(
    var frames: Int = 0,
    var badFrames: Int = 0,
    var tornFrames: Int = 0,
    var lostFrames: Int = 0,
    var fps: Double = 0.0,
    var rxBytes: Long = 0
)

/**
 * TCP 服务器（Android 扮演 Qt 的角色）：监听端口、接受 ESP32 拨入、读流喂解析器。
 * 关键分层：网络+解析+伪彩渲染 全在后台线程完成（双缓冲的"接收缓冲区"），
 * 只把渲染好的 Bitmap（完整帧）经 Handler 投递到主线程交换显示 —— UI 线程不阻塞、不撕裂。
 */
class TcpFrameServer(
    private val port: Int,
    private val paletteProvider: () -> ColorMap.Palette,
    private val listener: Listener
) {
    interface Listener {
        fun onStatus(msg: String)
        fun onClientConnected(ip: String)
        fun onClientDisconnected()
        fun onFrame(bmp: Bitmap, frame: ThermalFrame, minC: Double, maxC: Double, avgC: Double, stats: Stats)
    }

    private val ui = Handler(Looper.getMainLooper())
    @Volatile private var running = false
    private var thread: Thread? = null
    private var serverSocket: ServerSocket? = null
    private var clientSocket: Socket? = null

    // 统计 + 渲染状态（只在后台线程访问）
    private val stats = Stats()
    private val gate = FrameGate()
    private var lastFrameId = -1
    private var fpsWindowStart = 0L
    private var fpsWindowFrames = 0

    fun isRunning() = running

    fun start() {
        if (running) return
        running = true
        thread = Thread({ runServer() }, "tcp-frame-server").apply { isDaemon = true; start() }
    }

    fun stop() {
        running = false
        try { clientSocket?.close() } catch (_: Exception) {}
        try { serverSocket?.close() } catch (_: Exception) {}
        thread?.interrupt()
        thread = null
    }

    private fun runServer() {
        try {
            val ss = ServerSocket()
            ss.reuseAddress = true
            ss.bind(InetSocketAddress(port))
            serverSocket = ss
            postStatus("监听中 0.0.0.0:$port，等待 ESP32 接入…")
            while (running) {
                val sock = try { ss.accept() } catch (e: Exception) { break }
                clientSocket = sock
                try { sock.tcpNoDelay = true } catch (_: Exception) {}
                val ip = (sock.remoteSocketAddress as? InetSocketAddress)?.address?.hostAddress ?: "?"
                ui.post { listener.onClientConnected(ip) }
                resetSession()
                handleClient(sock)
                ui.post { listener.onClientDisconnected() }
                clientSocket = null
                try { sock.close() } catch (_: Exception) {}
                if (running) postStatus("客户端断开，等待重连…")
            }
        } catch (e: Exception) {
            if (running) postStatus("服务器错误：${e.message}")
        } finally {
            try { serverSocket?.close() } catch (_: Exception) {}
        }
    }

    private fun resetSession() {
        stats.frames = 0; stats.badFrames = 0; stats.tornFrames = 0
        stats.lostFrames = 0; stats.fps = 0.0; stats.rxBytes = 0
        lastFrameId = -1; fpsWindowFrames = 0; fpsWindowStart = System.currentTimeMillis()
    }

    private fun handleClient(sock: Socket) {
        val input = sock.getInputStream()
        val buf = ByteArray(1 shl 15) // 32KB
        val parser = FrameParser(
            onFrame = { f -> handleFrame(f) },
            onBad = { stats.badFrames++ }
        )
        while (running) {
            val n = try { input.read(buf) } catch (e: Exception) { break }
            if (n < 0) break
            stats.rxBytes += n
            parser.feed(buf, n)
        }
    }

    private fun handleFrame(f: ThermalFrame) {
        // 丢帧检测（frameId 回绕 16 位）
        if (lastFrameId >= 0) {
            val expected = (lastFrameId + 1) and 0xFFFF
            if (f.frameId != expected) {
                val gap = ((f.frameId - expected) and 0xFFFF)
                if (gap in 1..1000) stats.lostFrames += gap
            }
        }
        lastFrameId = f.frameId
        stats.frames++

        // 撕裂门禁：拒显则不交换显示缓冲，保留上一帧
        val g = gate.accept(f.pixels, f.width, f.height)
        if (!g.display) { stats.tornFrames++; return }

        // FPS（按实际显示帧计）
        val now = System.currentTimeMillis()
        fpsWindowFrames++
        val elapsed = now - fpsWindowStart
        if (elapsed >= 1000) {
            stats.fps = fpsWindowFrames * 1000.0 / elapsed
            fpsWindowFrames = 0
            fpsWindowStart = now
        }

        // 后台完成伪彩渲染（生成全新 Bitmap = 后缓冲）
        val r = ColorMap.render(f.pixels, paletteProvider())
        val bmp = Bitmap.createBitmap(f.width, f.height, Bitmap.Config.ARGB_8888)
        bmp.setPixels(r.argb, 0, f.width, 0, 0, f.width, f.height)

        val snapshot = stats.copy()
        ui.post { listener.onFrame(bmp, f, r.minC, r.maxC, r.avgC, snapshot) }
    }

    private fun postStatus(msg: String) = ui.post { listener.onStatus(msg) }
}
