/**
 * @file    ble_config.c
 * @brief   BLE GATT 服务器 — NimBLE 实现，BLE 专用帧头 E1/E2
 *
 * ── 功能 ──
 *   - 广播 "XRay_BLE"
 *   - GATT Service: RX (Write) / TX (Notify)
 *   - RX 接收 4 字节协议帧（帧头 0xE1）→ protocol_process(PROTO_BLE, ...)
 *   - TX 推送 4 字节响应帧（帧头 0xE2）
 *   - WiFi 用 F1/F2，BLE 用 E1/E2，协议层自动区分
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "ble_config.h"
#include "protocol.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble";

static uint16_t tx_attr_handle;

/* ── UUID ── */
static const ble_uuid128_t gatt_svc_uuid = BLE_UUID128_INIT(BLE_SVC_UUID);
static const ble_uuid128_t rx_char_uuid  = BLE_UUID128_INIT(BLE_RX_CHR_UUID);
static const ble_uuid128_t tx_char_uuid  = BLE_UUID128_INIT(BLE_TX_CHR_UUID);

/* ═══════════════════════════════════════════════
 *  TX 特征 — 读取返回 0，响应走 Notify 推送
 * ═══════════════════════════════════════════════ */

static int tx_char_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        static const uint8_t zero = 0;
        return os_mbuf_append(ctxt->om, &zero, 1);
    }
    return 0;
}

/* ═══════════════════════════════════════════════
 *  RX 特征 — 收到 4 字节 → protocol_process(PROTO_BLE)
 *  响应通过 TX Notify 推送（需客户端先订阅）
 * ═══════════════════════════════════════════════ */

static int rx_char_access(uint16_t conn, uint16_t attr,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && ctxt->om) {

        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len < 1) return 0;

        uint8_t rx_buf[FRAME_LEN] = {0};
        int n = (len > FRAME_LEN) ? FRAME_LEN : len;
        os_mbuf_copydata(ctxt->om, 0, n, rx_buf);

        ESP_LOGI(TAG, "收到: %02X %02X %02X %02X",
                 rx_buf[0], rx_buf[1], rx_buf[2], rx_buf[3]);

        uint8_t tx_buf[FRAME_LEN] = {0};
        int resp = protocol_process(PROTO_BLE, rx_buf, len, tx_buf);

        if (resp > 0) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(tx_buf, FRAME_LEN);
            if (om) {
                int rc = ble_gatts_notify_custom(conn, tx_attr_handle, om);
                if (rc == 0) {
                    ESP_LOGI(TAG, "通知响应: %02X %02X %02X %02X",
                             tx_buf[0], tx_buf[1], tx_buf[2], tx_buf[3]);
                } else {
                    ESP_LOGW(TAG, "通知失败(未订阅?): %d", rc);
                }
            }
        }
    }
    return 0;
}

/* ═══════════════════════════════════════════════
 *  GATT Service — NimBLE 自动为 NOTIFY 特征添加 CCCD
 * ═══════════════════════════════════════════════ */

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &rx_char_uuid.u,
                .access_cb = rx_char_access,
                .flags = BLE_GATT_CHR_F_WRITE,          /* 客户端写入协议帧 */
            },
            {
                .uuid = &tx_char_uuid.u,
                .access_cb = tx_char_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,         /* 服务端推送响应帧 */
                .val_handle = &tx_attr_handle,          /* 记录句柄用于通知 */
            },
            { 0 }
        },
    },
    { 0 }
};

/* ═══════════════════════════════════════════════
 *  GAP 事件 — 连接/断开/MTU
 * ═══════════════════════════════════════════════ */

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "客户端已连接");
        } else {
            ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL,
                              BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "客户端断开，恢复广播");
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL,
                          BLE_HS_FOREVER, NULL, ble_gap_event, NULL);
        return 0;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%d", event->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* ═══════════════════════════════════════════════
 *  广播 — 设备名 XRay_BLE
 * ═══════════════════════════════════════════════ */

static void ble_start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.name = (uint8_t *)BLE_DEVICE_NAME;
    fields.name_len = strlen(BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_gap_adv_params adv_params = {0};
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL,
                                BLE_HS_FOREVER, &adv_params,
                                ble_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE 广播已启动: %s", BLE_DEVICE_NAME);
    } else {
        ESP_LOGE(TAG, "广播失败: %d", rc);
    }
}

/* ═══════════════════════════════════════════════
 *  NimBLE 回调
 * ═══════════════════════════════════════════════ */

static void ble_on_sync(void)
{
    ble_gatts_find_chr(&gatt_svc_uuid.u,
                        &tx_char_uuid.u, NULL, &tx_attr_handle);
    ble_start_advertising();
}

static void ble_on_reset(int reason)
{
    ESP_LOGI(TAG, "控制器复位: %d", reason);
}

static void nimble_host_task(void *arg)
{
    nimble_port_run();
    vTaskDelete(NULL);
}

/* ═══════════════════════════════════════════════
 *  对外入口
 * ═══════════════════════════════════════════════ */

void ble_init(void)
{
    /* 回调必须先于 nimble_port_init 设置 */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE 初始化失败: %d", ret);
        return;
    }

    ble_svc_gap_init();             /* GAP 基础服务 */
    ble_svc_gatt_init();            /* GATT 基础服务 */
    ble_gatts_count_cfg(gatt_svr_svcs);
    ble_gatts_add_svcs(gatt_svr_svcs);
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    nimble_port_freertos_init(nimble_host_task);
}
