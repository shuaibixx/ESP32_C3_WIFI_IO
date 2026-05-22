/**
 * @file    protocol.c
 * @brief   协议解析 + IO 控制 — WiFi/BLE 双传输层共用
 *
 * ── 要点 ──
 *   - 帧长固定 4 字节，XOR 校验
 *   - WiFi 请求头 0xF1 → 响应头 0xF2
 *   - BLE  请求头 0xE1 → 响应头 0xE2
 *   - 同一帧格式解析逻辑，传输层间完全解耦
 */

#include "protocol.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "protocol";

/* ───────────────────────────────────────────────────
 *  IO 初始化
 * ─────────────────────────────────────────────────── */

void io_init(void)
{
    gpio_reset_pin(IO_PIN);
    gpio_set_direction(IO_PIN, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(IO_PIN, 0);

    ESP_LOGI(TAG, "IO 初始化完成: GPIO%d = 低电平", IO_PIN);
}

/* ───────────────────────────────────────────────────
 *  IO 控制
 * ─────────────────────────────────────────────────── */

void io_set_from_byte(uint8_t data)
{
    int level = (data >> IO_DATA_BIT) & 0x01;
    gpio_set_level(IO_PIN, level);

    ESP_LOGI(TAG, "设置 GPIO%d = %d (DATA=0x%02X)",
             IO_PIN, level, data);
}

uint8_t io_read_to_byte(void)
{
    int level = gpio_get_level(IO_PIN);
    return (uint8_t)(level & IO_DATA_MASK);
}

/* ───────────────────────────────────────────────────
 *  协议解析
 * ─────────────────────────────────────────────────── */

int protocol_process(int transport, const uint8_t *rx_frame, int rx_len, uint8_t *tx_frame)
{
    if (rx_len < FRAME_LEN)
        return 0;

    /* ── 传输层对应帧头：WiFi→F1/F2，BLE→E1/E2 ── */
    uint8_t expected_rx = (transport == PROTO_WIFI) ? PROTO_HEADER_WIFI_RX : PROTO_HEADER_BLE_RX;
    uint8_t response_hdr = (transport == PROTO_WIFI) ? PROTO_HEADER_WIFI_TX : PROTO_HEADER_BLE_TX;

    /* 帧头校验：非本传输层帧头 → 跳过 */
    if (rx_frame[0] != expected_rx)
        return 0;

    uint8_t cmd  = rx_frame[1];
    uint8_t data = rx_frame[2];
    uint8_t chk  = rx_frame[3];

    /* XOR 校验 */
    uint8_t calc_chk = (uint8_t)(cmd ^ data);
    if (chk != calc_chk) {
        ESP_LOGW(TAG, "校验错误! 计算值=0x%02X, 接收值=0x%02X",
                 calc_chk, chk);
        return 0;
    }

    uint8_t resp_data = 0;

    /* ── 命令分发 ── */
    switch (cmd) {

    case CMD_SET_GPIO:                     /* 设置 IO 输出 */
        io_set_from_byte(data);
        resp_data = io_read_to_byte();     /* 回读实际电平 */
        break;

    case CMD_READ_GPIO:                    /* 只读 IO 状态 */
        resp_data = io_read_to_byte();
        break;

    case CMD_RESET_WIFI: {                /* 清除 WiFi 凭据 */
        nvs_handle_t handle;
        if (nvs_open("wifi", NVS_READWRITE, &handle) == ESP_OK) {
            nvs_erase_all(handle);
            nvs_commit(handle);
            nvs_close(handle);
            ESP_LOGI(TAG, "WiFi 凭据已清除，1 秒后重启...");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        break;
    }

    default:
        ESP_LOGW(TAG, "未知命令: 0x%02X", cmd);
        return 0;
    }

    /* ── 组装响应帧 ── */
    tx_frame[0] = response_hdr;                     /* 传输层对应的响应帧头 */
    tx_frame[1] = cmd;                              /* 回显命令 */
    tx_frame[2] = resp_data;                        /* IO 当前状态 */
    tx_frame[3] = (uint8_t)(cmd ^ resp_data);       /* 重算校验 */

    ESP_LOGI(TAG, "响应(%s): [%02X %02X %02X %02X]",
             (transport == PROTO_WIFI) ? "WiFi" : "BLE",
             tx_frame[0], tx_frame[1], tx_frame[2], tx_frame[3]);

    return FRAME_LEN;
}
