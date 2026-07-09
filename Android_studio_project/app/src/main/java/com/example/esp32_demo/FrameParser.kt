package com.example.esp32_demo

/**
 * 字节流 → 完整帧 状态机，移植自 Qt 端 frameparser.cpp。
 * 只有整帧收齐且校验通过才回调 onFrame（不完整帧永远不外泄，满足"帧完整优先"）。
 * 校验容错：接受 带/不带 sync 之和 × 大端/小端 接收值，与 Qt 一致。
 */
class FrameParser(
    private val onFrame: (ThermalFrame) -> Unit,
    private val onBad: () -> Unit
) {
    private enum class State { SYNC0, SYNC1, HEADER, PAYLOAD, CHECKSUM }

    private var state = State.SYNC0
    private val header = ByteArray(Protocol.HEADER_AFTER_SYNC)
    private var headerPos = 0
    private var payload = ByteArray(Protocol.PAYLOAD)
    private var payloadPos = 0
    private val checksum = ByteArray(Protocol.CHECKSUM_SIZE)
    private var checksumPos = 0

    private var frameId = 0
    private var width = 0
    private var height = 0
    private var pixelLen = 0
    private var sumWithSync = 0   // sync0..payload 累加
    private var sumNoSync = 0     // header..payload 累加（不含 sync 两字节）

    fun reset() {
        state = State.SYNC0
        headerPos = 0; payloadPos = 0; checksumPos = 0
    }

    fun feed(data: ByteArray, len: Int) {
        var i = 0
        while (i < len) {
            val b = data[i].toInt() and 0xFF
            when (state) {
                State.SYNC0 -> if (b == Protocol.SYNC0) {
                    sumWithSync = b; state = State.SYNC1
                }
                State.SYNC1 -> when (b) {
                    Protocol.SYNC1 -> { sumWithSync += b; sumNoSync = 0; headerPos = 0; state = State.HEADER }
                    Protocol.SYNC0 -> sumWithSync = b            // AA AA … 保持等待 55
                    else -> state = State.SYNC0
                }
                State.HEADER -> {
                    header[headerPos++] = b.toByte(); sumWithSync += b; sumNoSync += b
                    if (headerPos == Protocol.HEADER_AFTER_SYNC) finishHeader()
                }
                State.PAYLOAD -> {
                    payload[payloadPos++] = b.toByte(); sumWithSync += b; sumNoSync += b
                    if (payloadPos == pixelLen) { checksumPos = 0; state = State.CHECKSUM }
                }
                State.CHECKSUM -> {
                    checksum[checksumPos++] = b.toByte()
                    if (checksumPos == Protocol.CHECKSUM_SIZE) { finishChecksum(); state = State.SYNC0 }
                }
            }
            i++
        }
    }

    private fun finishHeader() {
        val type = header[0].toInt() and 0xFF
        frameId = u16(header[1], header[2])
        width = u16(header[3], header[4])
        height = u16(header[5], header[6])
        pixelLen = (u16(header[7], header[8]) shl 16) or u16(header[9], header[10])
        val ok = type == Protocol.TYPE_RAW16 &&
                width in 1..640 && height in 1..480 &&
                pixelLen == width * height * 2
        if (!ok) { onBad(); state = State.SYNC0; return }
        if (payload.size != pixelLen) payload = ByteArray(pixelLen)
        payloadPos = 0
        state = State.PAYLOAD
    }

    private fun finishChecksum() {
        val recvBe = u16(checksum[0], checksum[1])
        val recvLe = u16(checksum[1], checksum[0])
        val sw = sumWithSync and 0xFFFF
        val sn = sumNoSync and 0xFFFF
        if (recvBe == sw || recvLe == sw || recvBe == sn || recvLe == sn) emitFrame() else onBad()
    }

    private fun emitFrame() {
        val n = width * height
        val px = IntArray(n)
        var j = 0
        for (k in 0 until n) {
            px[k] = ((payload[j].toInt() and 0xFF) shl 8) or (payload[j + 1].toInt() and 0xFF)
            j += 2
        }
        onFrame(ThermalFrame(frameId, width, height, px))
    }

    private fun u16(hi: Byte, lo: Byte) = ((hi.toInt() and 0xFF) shl 8) or (lo.toInt() and 0xFF)
}
