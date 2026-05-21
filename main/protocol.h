/**
 * @file    protocol.h
 * @brief   自定义通信协议 & 单 IO 控制
 *
 * 协议帧格式 (4 字节):
 *   ┌────────┬────────┬────────┬──────────┐
 *   │ HEADER │  CMD   │  DATA  │ CHECKSUM │
 *   │0xF1/F2 │  命令  │  数据  │  校验值  │
 *   └────────┴────────┴────────┴──────────┘
 *
 *   HEADER:   接收帧头 0xF1 (PC→ESP32)，发送帧头 0xF2 (ESP32→PC)
 *   CMD:      0x01 = 设置 IO (CMD_SET_GPIO)
 *             0x02 = 读取 IO (CMD_READ_GPIO, 只读不写)
 *   DATA:     bit0 = IO 电平 (0=低电平, 1=高电平), bit1~7 预留
 *   CHECKSUM: CMD XOR DATA
 *
 * 控制引脚: GPIO0
 */

#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>
#include "driver/gpio.h"

/* ── 协议常量 ── */
#define PROTO_HEADER_RX 0xF1    /* 接收帧头 (PC→ESP32) */
#define PROTO_HEADER_TX 0xF2    /* 发送帧头 (ESP32→PC) */
#define FRAME_LEN       4       /* 一帧固定 4 字节 */
#define CMD_SET_GPIO    0x01    /* 命令：设置 GPIO 输出 */
#define CMD_READ_GPIO   0x02    /* 命令：读取 GPIO 状态 */
#define CMD_RESET_WIFI  0x03    /* 命令：清除 WiFi 凭据并重启 */

/* ── IO 控制引脚定义 ── */
#define IO_PIN          GPIO_NUM_0      /* 受控 IO 口: GPIO0 */
#define IO_DATA_MASK    0x01            /* DATA 字节位掩码: 仅 bit0 有效 */
#define IO_DATA_BIT     0               /* 有效位: 第 0 位 */

/* ── 函数声明 ── */

/**
 * @brief   初始化 IO 引脚为输出（默认低电平）
 */
void io_init(void);

/**
 * @brief   根据协议 DATA 字节设置 IO 电平
 * @param   data : 协议帧中的 DATA 字节，仅 bit0 有效
 *                 bit0 = 0 → IO 输出低电平
 *                 bit0 = 1 → IO 输出高电平
 */
void io_set_from_byte(uint8_t data);

/**
 * @brief   读取 IO 当前电平，打包为协议 DATA 字节
 * @retval  bit0 = IO 当前电平，bit1~7 = 0
 */
uint8_t io_read_to_byte(void);

/**
 * @brief   处理一帧协议数据
 * @param   rx_frame : 接收到的 4 字节帧
 * @param   rx_len   : 接收长度（需 >= FRAME_LEN）
 * @param   tx_frame : 输出的响应帧缓冲区（调用方提供 4 字节空间）
 * @retval  FRAME_LEN (4): 成功处理，tx_frame 中为有效响应
 *          0             : 校验失败或命令不支持，tx_frame 无效
 */
int protocol_process(const uint8_t *rx_frame, int rx_len, uint8_t *tx_frame);

#endif /* __PROTOCOL_H__ */
