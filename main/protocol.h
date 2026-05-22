/**
 * @file    protocol.h
 * @brief   自定义 4 字节通信协议 — WiFi / BLE 传输层共用
 *
 * ── 帧格式 (定长 4 字节) ──
 *   ┌────────┬────────┬────────┬──────────┐
 *   │ HEADER │  CMD   │  DATA  │ CHECKSUM │
 *   │传输层标识│ 命令   │ 数据   │ CMD^DATA │
 *   └────────┴────────┴────────┴──────────┘
 *
 * ── HEADER (Byte 0) — 传输层区分 ──
 *   WiFi 请求: 0xF1 (客户端 → ESP32)
 *   WiFi 响应: 0xF2 (ESP32 → 客户端)
 *   BLE  请求: 0xE1 (客户端 → ESP32)
 *   BLE  响应: 0xE2 (ESP32 → 客户端)
 *   请求/响应帧头一一配对，收到 F1 回 F2，收到 E1 回 E2
 *
 * ── CMD (Byte 1) ──
 *   0x01 = CMD_SET_GPIO     设置 GPIO0 电平
 *   0x02 = CMD_READ_GPIO    读取 GPIO0 电平（只读，不改变输出）
 *   0x03 = CMD_RESET_WIFI   擦除 NVS WiFi 凭据并重启
 *
 * ── DATA (Byte 2) ──
 *   bit0   = GPIO0 电平 (0=低, 1=高)
 *   bit1~7 = 保留，填 0
 *
 * ── CHECKSUM (Byte 3) ──
 *   CMD XOR DATA，接收方重算比对，不匹配则丢弃
 *
 * ── 控制引脚 ──
 *   GPIO0 (GPIO_MODE_INPUT_OUTPUT, 默认低电平)
 */

#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>
#include "driver/gpio.h"

/* ── 传输层标识 ── */
#define PROTO_WIFI       0
#define PROTO_BLE        1

/* ── WiFi 帧头 ── */
#define PROTO_HEADER_WIFI_RX  0xF1
#define PROTO_HEADER_WIFI_TX  0xF2

/* ── BLE 帧头 ── */
#define PROTO_HEADER_BLE_RX   0xE1
#define PROTO_HEADER_BLE_TX   0xE2

/* ── 兼容旧代码 ── */
#define PROTO_HEADER_RX  PROTO_HEADER_WIFI_RX
#define PROTO_HEADER_TX  PROTO_HEADER_WIFI_TX

/* ── 协议常量 ── */
#define FRAME_LEN       4
#define CMD_SET_GPIO    0x01    /* 设置 GPIO 输出 */
#define CMD_READ_GPIO   0x02    /* 读取 GPIO 状态（只读不写） */
#define CMD_RESET_WIFI  0x03    /* 清除 WiFi 凭据并重启 */

/* ── IO 控制引脚 ── */
#define IO_PIN          GPIO_NUM_0
#define IO_DATA_MASK    0x01
#define IO_DATA_BIT     0

/* ── 函数声明 ── */

void io_init(void);
void io_set_from_byte(uint8_t data);
uint8_t io_read_to_byte(void);

/**
 * @brief   处理一帧协议数据（传输层无关）
 * @param   transport : PROTO_WIFI 或 PROTO_BLE，用于帧头校验和响应帧头匹配
 * @param   rx_frame  : 接收到的 4 字节帧
 * @param   rx_len    : 接收长度（需 >= FRAME_LEN）
 * @param   tx_frame  : 输出的响应帧缓冲区（4 字节）
 * @retval  FRAME_LEN (4) : 成功
 *          0              : 校验/帧头/命令无效
 */
int protocol_process(int transport, const uint8_t *rx_frame, int rx_len, uint8_t *tx_frame);

#endif /* __PROTOCOL_H__ */
