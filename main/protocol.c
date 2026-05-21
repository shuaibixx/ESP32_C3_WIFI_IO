/**
 * @file    protocol.c
 * @brief   协议解析 + IO 控制实现
 *
 * 说明:
 *   - 仅控制 1 个 IO (GPIO0)，通过 DATA 字节的 bit0 设置
 *   - bit1~bit7 读取后忽略，仅作预留
 *   - 异或校验: CHECKSUM = CMD ^ DATA
 *   - 接收帧头 0xF1，发送帧头 0xF2
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

int protocol_process(const uint8_t *rx_frame, int rx_len, uint8_t *tx_frame)
{
    if (rx_len < FRAME_LEN)
        return 0;

    if (rx_frame[0] != PROTO_HEADER_RX)
        return 0;

    uint8_t cmd  = rx_frame[1];
    uint8_t data = rx_frame[2];
    uint8_t chk  = rx_frame[3];

    uint8_t calc_chk = (uint8_t)(cmd ^ data);
    if (chk != calc_chk) {
        ESP_LOGW(TAG, "校验错误! 计算值=0x%02X, 接收值=0x%02X",
                 calc_chk, chk);
        return 0;
    }

    uint8_t resp_data = 0;

    switch (cmd) {

    case CMD_SET_GPIO:
        io_set_from_byte(data);
        resp_data = io_read_to_byte();
        break;

    case CMD_READ_GPIO:
        resp_data = io_read_to_byte();
        break;

    case CMD_RESET_WIFI: {
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

    tx_frame[0] = PROTO_HEADER_TX;
    tx_frame[1] = cmd;
    tx_frame[2] = resp_data;
    tx_frame[3] = (uint8_t)(cmd ^ resp_data);

    ESP_LOGI(TAG, "响应: [%02X %02X %02X %02X]",
             tx_frame[0], tx_frame[1], tx_frame[2], tx_frame[3]);

    return FRAME_LEN;
}
