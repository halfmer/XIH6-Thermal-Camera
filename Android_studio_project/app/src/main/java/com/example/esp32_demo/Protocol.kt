package com.example.esp32_demo

/**
 * 热成像 AA55 帧协议常量（与 STM32 lepton_stream.c / ESP32 sketch / Qt frameparser 对齐）。
 *
 * 线格式（多字节大端）：
 *   [0]=0xAA [1]=0x55 | type(1)=0x01 | frameId(u16) | width(u16) | height(u16)
 *   | pixelLen(u32)=w*h*2 | pixels(N=38400, u16 BE, 厘开尔文) | checksum(u16)
 * 完整一帧 = 2 + 11 + 38400 + 2 = 38415 字节。
 * checksum = 从 sync0 到最后一个像素字节的 8-bit 逐字节累加和（低 16 位）。
 */
object Protocol {
    const val SYNC0 = 0xAA
    const val SYNC1 = 0x55
    const val TYPE_RAW16 = 0x01
    const val WIDTH = 160
    const val HEIGHT = 120
    const val HEADER_AFTER_SYNC = 11          // type1 + id2 + w2 + h2 + len4
    const val CHECKSUM_SIZE = 2
    const val PAYLOAD = WIDTH * HEIGHT * 2     // 38400
    const val FRAME_LEN = 2 + HEADER_AFTER_SYNC + PAYLOAD + CHECKSUM_SIZE // 38415
    const val DEFAULT_PORT = 8888
}

/** 一帧完整、已校验的热成像数据。pixels 为原生无符号 16-bit（0..65535），长度 = width*height。 */
class ThermalFrame(
    val frameId: Int,
    val width: Int,
    val height: Int,
    val pixels: IntArray
)
