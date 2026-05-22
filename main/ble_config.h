/**
 * @file    ble_config.h
 * @brief   BLE GATT 服务器 — 协议帧控制 GPIO0
 *
 * ── 设备名 ──
 *   XRay_BLE（广播名，BLE 扫描可见）
 *
 * ── GATT Service ──
 *   UUID: 0F0E0D0C-0B0A-0908-0706-050403020100（128-bit 自定义）
 *     ├─ RX Characteristic (UUID 末位 ...0E10)：Write，客户端写入 4 字节协议帧
 *     └─ TX Characteristic (UUID 末位 ...0E11)：Notify，ESP32 推送 4 字节响应
 *
 * ── 协议帧 ──
 *   与 WiFi TCP 共用一个 protocol_process()，帧格式完全相同的 4 字节协议
 *
 * ── 连接 ──
 *   无配对加密（open），连接即用，断开自动恢复广播
 */

#ifndef __BLE_CONFIG_H__
#define __BLE_CONFIG_H__

#define BLE_DEVICE_NAME     "XRay_BLE"

/* 128-bit UUID: 0F0E0D0C-0B0A-0908-0706-0504030201xx */
#define BLE_SVC_UUID        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07, \
                            0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F  /* Service       */
#define BLE_RX_CHR_UUID     0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07, \
                            0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x10  /* RX: PC→ESP32  */
#define BLE_TX_CHR_UUID     0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07, \
                            0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x11  /* TX: ESP32→PC  */

void ble_init(void);

#endif /* __BLE_CONFIG_H__ */
