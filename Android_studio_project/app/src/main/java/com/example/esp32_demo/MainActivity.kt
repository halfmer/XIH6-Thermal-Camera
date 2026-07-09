package com.example.esp32_demo

import android.graphics.Bitmap
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.google.android.material.button.MaterialButton
import com.google.android.material.switchmaterial.SwitchMaterial
import java.net.Inet4Address
import java.net.NetworkInterface
import java.text.SimpleDateFormat
import java.util.Collections
import java.util.Date
import java.util.Locale

/**
 * 主界面：Android 作为 TCP 服务器监听 8888，只显示已完整接收、校验并渲染完成的热成像帧。
 */
class MainActivity : AppCompatActivity(), TcpFrameServer.Listener {

    private lateinit var thermalView: ThermalView
    private lateinit var historyView: HistoryGraphView
    private lateinit var btnToggle: MaterialButton
    private lateinit var btnFreeze: MaterialButton
    private lateinit var spPalette: Spinner
    private lateinit var tvConnectionChip: TextView
    private lateinit var tvHeaderSub: TextView
    private lateinit var tvStatus: TextView
    private lateinit var tvStats: TextView
    private lateinit var tvMetricFrames: TextView
    private lateinit var tvMetricFps: TextView
    private lateinit var tvMetricQuality: TextView
    private lateinit var tvTempStats: TextView
    private lateinit var tvAlert: TextView
    private lateinit var tvThreshold: TextView
    private lateinit var tvLog: TextView
    private lateinit var swOverlay: SwitchMaterial
    private lateinit var swGrid: SwitchMaterial
    private lateinit var swSpot: SwitchMaterial
    private lateinit var swAlert: SwitchMaterial
    private lateinit var sbAlert: SeekBar

    private val ui = Handler(Looper.getMainLooper())
    private val logs = ArrayDeque<String>()
    private val logTime = SimpleDateFormat("HH:mm:ss", Locale.US)

    private var server: TcpFrameServer? = null
    private var palette = ColorMap.Palette.IRONBOW
    private var displayFrozen = false
    private var alertThresholdC = 60
    private var sessionStartMs = 0L
    private var clientIp = "--"

    private val uptimeTicker = object : Runnable {
        override fun run() {
            updateHeader()
            if (server?.isRunning() == true) ui.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        bindViews()
        setupPaletteSelector()
        setupDisplayTools()
        setupAlertControls()

        btnToggle.setOnClickListener { toggle() }
        btnFreeze.setOnClickListener { toggleFreeze() }
        showIdleStatus()
        addLog("应用启动")
    }

    private fun bindViews() {
        thermalView = findViewById(R.id.thermalView)
        historyView = findViewById(R.id.historyView)
        btnToggle = findViewById(R.id.btnToggle)
        btnFreeze = findViewById(R.id.btnFreeze)
        spPalette = findViewById(R.id.spPalette)
        tvConnectionChip = findViewById(R.id.tvConnectionChip)
        tvHeaderSub = findViewById(R.id.tvHeaderSub)
        tvStatus = findViewById(R.id.tvStatus)
        tvStats = findViewById(R.id.tvStats)
        tvMetricFrames = findViewById(R.id.tvMetricFrames)
        tvMetricFps = findViewById(R.id.tvMetricFps)
        tvMetricQuality = findViewById(R.id.tvMetricQuality)
        tvTempStats = findViewById(R.id.tvTempStats)
        tvAlert = findViewById(R.id.tvAlert)
        tvThreshold = findViewById(R.id.tvThreshold)
        tvLog = findViewById(R.id.tvLog)
        swOverlay = findViewById(R.id.swOverlay)
        swGrid = findViewById(R.id.swGrid)
        swSpot = findViewById(R.id.swSpot)
        swAlert = findViewById(R.id.swAlert)
        sbAlert = findViewById(R.id.sbAlert)
    }

    private fun setupPaletteSelector() {
        val palettes = ColorMap.Palette.values()
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, palettes.map { it.label })
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        spPalette.adapter = adapter
        spPalette.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: android.view.View?, pos: Int, id: Long) {
                palette = palettes[pos]
                addLog("调色板切换为 ${palette.label}")
            }

            override fun onNothingSelected(p: AdapterView<*>?) = Unit
        }
    }

    private fun setupDisplayTools() {
        swOverlay.setOnCheckedChangeListener { _, checked -> thermalView.setOverlayEnabled(checked) }
        swGrid.setOnCheckedChangeListener { _, checked -> thermalView.setGridEnabled(checked) }
        swSpot.setOnCheckedChangeListener { _, checked -> thermalView.setSpotEnabled(checked) }
    }

    private fun setupAlertControls() {
        updateThresholdFromSeekBar()
        sbAlert.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                updateThresholdFromSeekBar()
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        swAlert.setOnCheckedChangeListener { _, checked ->
            tvAlert.text = if (checked) "告警监控已启用" else "告警未启用"
            tvAlert.setTextColor(colorOf(if (checked) R.color.accent_warm else R.color.text_secondary))
            addLog(if (checked) "温度告警启用" else "温度告警关闭")
        }
    }

    private fun updateThresholdFromSeekBar() {
        alertThresholdC = 20 + sbAlert.progress
        tvThreshold.text = "阈值 ${alertThresholdC}°C"
    }

    private fun toggle() {
        val s = server
        if (s != null && s.isRunning()) {
            s.stop()
            server = null
            btnToggle.text = "启动监听"
            displayFrozen = false
            btnFreeze.text = "冻结"
            thermalView.clear()
            historyView.clear()
            sessionStartMs = 0L
            clientIp = "--"
            ui.removeCallbacks(uptimeTicker)
            showIdleStatus()
            addLog("监听已停止")
        } else {
            val srv = TcpFrameServer(Protocol.DEFAULT_PORT, { palette }, this)
            server = srv
            sessionStartMs = SystemClock.elapsedRealtime()
            srv.start()
            btnToggle.text = "停止监听"
            setConnectionChip("监听中", R.color.accent)
            updateHeader()
            ui.post(uptimeTicker)
            addLog("开始监听 0.0.0.0:${Protocol.DEFAULT_PORT}")
        }
    }

    private fun toggleFreeze() {
        displayFrozen = !displayFrozen
        btnFreeze.text = if (displayFrozen) "继续" else "冻结"
        tvStatus.text = if (displayFrozen) "画面已冻结；后台仍持续接收完整帧" else "画面已恢复实时显示"
        addLog(if (displayFrozen) "画面冻结" else "画面恢复")
    }

    private fun showIdleStatus() {
        val ips = localIps()
        setConnectionChip("待机", R.color.text_secondary)
        tvHeaderSub.text = "TCP ${Protocol.DEFAULT_PORT} · 完整帧显示"
        tvStatus.text = if (ips.isEmpty()) {
            "未连接 WiFi。请先加入 ESP32 所在网络（默认 SSID：ESP32_TEST）"
        } else {
            "本机 IP：${ips.joinToString(" / ")}\nESP32 默认连接 192.168.1.100:${Protocol.DEFAULT_PORT}，请确保本机是该 IP，或同步修改固件 SERVER_IP"
        }
        tvStats.text = "未开始监听"
        tvMetricFrames.text = "0"
        tvMetricFps.text = "0.0"
        tvMetricQuality.text = "100%"
        tvTempStats.text = "最高 --.-°C  最低 --.-°C  平均 --.-°C"
        tvAlert.text = if (swAlert.isChecked) "告警监控已启用" else "告警未启用"
    }

    private fun updateHeader() {
        val uptime = if (sessionStartMs == 0L) "00:00:00" else formatDuration(SystemClock.elapsedRealtime() - sessionStartMs)
        val ipText = localIps().joinToString(" / ").ifBlank { "无本机 IPv4" }
        tvHeaderSub.text = "TCP ${Protocol.DEFAULT_PORT} · $ipText · $uptime · 客户端 $clientIp"
    }

    // ---- TcpFrameServer.Listener（均在主线程回调） ----

    override fun onStatus(msg: String) {
        tvStatus.text = msg
        if (msg.contains("监听中")) setConnectionChip("监听中", R.color.accent)
        addLog(msg)
    }

    override fun onClientConnected(ip: String) {
        clientIp = ip
        setConnectionChip("在线", R.color.ok)
        tvStatus.text = "ESP32 已接入：$ip"
        updateHeader()
        addLog("客户端接入 $ip")
    }

    override fun onClientDisconnected() {
        clientIp = "--"
        thermalView.clear()
        setConnectionChip(if (server?.isRunning() == true) "监听中" else "待机", R.color.accent)
        updateHeader()
        addLog("客户端断开")
    }

    override fun onFrame(
        bmp: Bitmap, frame: ThermalFrame, minC: Double, maxC: Double, avgC: Double, stats: Stats
    ) {
        if (!displayFrozen) {
            thermalView.show(bmp, frame, minC, maxC, avgC, stats.fps)
        }
        historyView.addSample(stats.fps, minC, maxC, avgC)
        tvMetricFrames.text = stats.frames.toString()
        tvMetricFps.text = "%.1f".format(stats.fps)
        tvMetricQuality.text = "${frameQuality(stats)}%"
        tvTempStats.text = "最高 ${"%.1f".format(maxC)}°C  最低 ${"%.1f".format(minC)}°C  平均 ${"%.1f".format(avgC)}°C"
        updateAlert(maxC)
        tvStats.text = "帧 ${stats.frames}  丢 ${stats.lostFrames}  错 ${stats.badFrames}  " +
                "撕裂拒显 ${stats.tornFrames}  ${frame.width}×${frame.height}  " +
                "FPS ${"%.1f".format(stats.fps)}  RX ${formatBytes(stats.rxBytes)}"
    }

    override fun onDestroy() {
        super.onDestroy()
        ui.removeCallbacks(uptimeTicker)
        server?.stop()
        server = null
    }

    private fun updateAlert(maxC: Double) {
        if (!swAlert.isChecked) return
        if (maxC >= alertThresholdC) {
            tvAlert.text = "高温告警：最高 ${"%.1f".format(maxC)}°C ≥ ${alertThresholdC}°C"
            tvAlert.setTextColor(colorOf(R.color.danger))
        } else {
            tvAlert.text = "温度正常：最高 ${"%.1f".format(maxC)}°C"
            tvAlert.setTextColor(colorOf(R.color.ok))
        }
    }

    private fun frameQuality(stats: Stats): Int {
        val rejected = stats.badFrames + stats.tornFrames + stats.lostFrames
        val total = (stats.frames + rejected).coerceAtLeast(1)
        return ((stats.frames * 100.0) / total).toInt().coerceIn(0, 100)
    }

    private fun setConnectionChip(text: String, colorRes: Int) {
        tvConnectionChip.text = text
        tvConnectionChip.setTextColor(colorOf(colorRes))
    }

    private fun addLog(message: String) {
        if (logs.size >= 6) logs.removeFirst()
        logs.addLast("${logTime.format(Date())}  $message")
        tvLog.text = logs.joinToString("\n")
    }

    private fun colorOf(colorRes: Int): Int = androidx.core.content.ContextCompat.getColor(this, colorRes)

    private fun formatBytes(bytes: Long): String {
        return if (bytes >= 1024 * 1024) {
            "%.1fMB".format(bytes / 1024.0 / 1024.0)
        } else {
            "${bytes / 1024}KB"
        }
    }

    private fun formatDuration(ms: Long): String {
        val totalSeconds = ms / 1000
        val h = totalSeconds / 3600
        val m = (totalSeconds % 3600) / 60
        val s = totalSeconds % 60
        return "%02d:%02d:%02d".format(h, m, s)
    }

    private fun localIps(): List<String> {
        val out = ArrayList<String>()
        try {
            for (nif in Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!nif.isUp || nif.isLoopback) continue
                for (addr in Collections.list(nif.inetAddresses)) {
                    if (!addr.isLoopbackAddress && addr is Inet4Address) {
                        addr.hostAddress?.let { out.add(it) }
                    }
                }
            }
        } catch (_: Exception) {
        }
        return out
    }
}
